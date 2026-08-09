#include "castlemist/ui/struct_tree.h"

#include <commctrl.h>

#include <cstdio>
#include <cwchar>
#include <unordered_map>

namespace castlemist::structtree {

namespace {

// Field/type names are ArenaNet identifiers (always ASCII), but a leaf's
// decoded valueString can be an embedded filename or cstring pulled straight
// out of the file's own text -- i.e. arbitrary UTF-8. A naive char->wchar_t
// widen mangles anything outside ASCII, so go through MultiByteToWideChar
// like the rest of the codebase (see core::bytes_to_wide) instead of the
// begin()/end() widen trick.
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), need);
    return w;
}

// Every node label is built the same way, so both the recursive inserter and
// any future "jump to node" search can share it:
//   name : typeName  = value                  (leaf)
//   name : typeName  [n children]              (group/struct/array)
// Offset/size are appended in a trailing "@0xOFFSET (SIZE bytes)" so a glance
// at a row tells you exactly where in the buffer it lives -- the same
// information the hex viewer's left column gives you, just organized by
// struct instead of by row.
std::wstring format_label(const ParsedNode& node) {
    wchar_t buf[512];
    std::wstring name = utf8_to_wide(node.name);
    std::wstring type = utf8_to_wide(node.typeName);

    if (node.isLeaf() && !node.valueString.empty()) {
        std::wstring value = utf8_to_wide(node.valueString);
        // Long decoded strings (e.g. embedded filenames) still need to stay
        // readable in a tree row; clip rather than let the control clip mid-word.
        if (value.size() > 180) {
            value.resize(180);
            value += L"...";
        }
        swprintf(buf, 512, L"%ls : %ls = %ls  @0x%zX (%zu bytes)", name.c_str(), type.c_str(), value.c_str(),
                 node.offset, node.size);
    } else {
        swprintf(buf, 512, L"%ls : %ls  [%zu]  @0x%zX (%zu bytes)", name.c_str(), type.c_str(), node.children.size(),
                 node.offset, node.size);
    }
    return buf;
}

// Recursion is bounded by BinaryParser's own depth guard (200) on the data
// side; the tree build no longer recurses at all (see insert_one_level), but
// the depth cap is kept as a guard against pathologically deep templates
// producing an unusable "expand forever" chain.
constexpr int kMaxInsertDepth = 64;
constexpr size_t kMaxChildrenPerNode = 20000;

// lParam of every real (non-placeholder) item is a ParsedNode* into the tree
// rooted at s_roots[hwnd] below -- valid for as long as that root is kept
// alive, which set_tree/clear/set_message take care of. Placeholder
// "loading" stand-ins carry lParam 0 so TVN_ITEMEXPANDING can tell a real
// node from a not-yet-expanded one.
constexpr LPARAM kPlaceholderParam = 0;

// One parsed tree can be selected, browsed a while, then replaced by the next
// selection. Children are only materialized as the user expands nodes, so
// the ParsedNode tree itself has to outlive that browsing -- keep the root
// shared_ptr alive here, keyed by control HWND (there is exactly one struct
// tree control in the app, but keying by HWND costs nothing and avoids a
// global singleton assumption leaking into this file).
std::unordered_map<HWND, ParsedNodePtr> s_roots;

// Inserts a placeholder "Loading..." child under `parent` so the tree
// control draws an expand glyph, without walking into `node`'s own children.
// Real population happens in expand_node() the first time the user opens it.
void insert_placeholder(HWND tree, HTREEITEM parent) {
    TVINSERTSTRUCTW is{};
    is.hParent = parent;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | TVIF_PARAM;
    is.item.pszText = const_cast<wchar_t*>(L"Loading...");
    is.item.lParam = kPlaceholderParam;
    TreeView_InsertItem(tree, &is);
}

// Inserts exactly one level of real children under `parent` (`node`'s direct
// children), each carrying a ParsedNode* lParam. Any child that itself has
// children gets a placeholder rather than being walked further -- that's the
// whole lazy-load contract: one level of real work per expand, however big
// the subtree below it is.
void insert_one_level(HWND tree, HTREEITEM parent, const ParsedNode& node, int depth) {
    if (depth > kMaxInsertDepth) {
        TVINSERTSTRUCTW is{};
        is.hParent = parent;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT;
        is.item.pszText = const_cast<wchar_t*>(L"(max tree depth reached)");
        TreeView_InsertItem(tree, &is);
        return;
    }

    size_t count = node.children.size();
    bool truncated = count > kMaxChildrenPerNode;
    if (truncated) count = kMaxChildrenPerNode;

    for (size_t i = 0; i < count; ++i) {
        const ParsedNodePtr& child = node.children[i];
        if (!child) continue;

        std::wstring label = format_label(*child);

        TVINSERTSTRUCTW is{};
        is.hParent = parent;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        is.item.pszText = const_cast<wchar_t*>(label.c_str());
        // Stash the node itself (not just the offset) so expand_node() can
        // read it back later, and a future "sync with hex view" click
        // handler has the full node -- offset, size, decoded value -- to
        // work from without re-walking the tree.
        is.item.lParam = reinterpret_cast<LPARAM>(child.get());
        // Tell the control up front whether to draw an expand glyph, so a
        // leaf never shows a [+] it can't back up (TreeView_InsertItem alone
        // infers this from actual children, and this node has none yet).
        is.item.cChildren = child->children.empty() ? 0 : 1;
        HTREEITEM item = TreeView_InsertItem(tree, &is);

        if (!child->children.empty()) {
            insert_placeholder(tree, item);
        }
    }

    if (truncated) {
        TVINSERTSTRUCTW is{};
        is.hParent = parent;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT;
        wchar_t buf[96];
        swprintf(buf, 96, L"(%zu more, truncated)", node.children.size() - kMaxChildrenPerNode);
        is.item.pszText = buf;
        TreeView_InsertItem(tree, &is);
    }
}

// Replaces `item`'s single placeholder child with the real one-level
// expansion of the ParsedNode stashed in its lParam. No-op if the item isn't
// a real node, or has already been expanded for real (re-collapsing and
// re-expanding a node should not redo the work or duplicate items).
void expand_node(HWND tree, HTREEITEM item) {
    TVITEMW tvi{};
    tvi.mask = TVIF_PARAM;
    tvi.hItem = item;
    if (!TreeView_GetItem(tree, &tvi) || tvi.lParam == kPlaceholderParam) {
        return;
    }
    const auto* node = reinterpret_cast<const ParsedNode*>(tvi.lParam);
    if (node == nullptr) {
        return;
    }

    // Already expanded for real? Detect by checking whether the first child
    // is still the placeholder -- if so it carries lParam 0; any real node
    // (even one with zero children of its own, which never gets a
    // placeholder in the first place) means this was already done.
    HTREEITEM first_child = TreeView_GetChild(tree, item);
    if (first_child != nullptr) {
        TVITEMW child_tvi{};
        child_tvi.mask = TVIF_PARAM;
        child_tvi.hItem = first_child;
        if (TreeView_GetItem(tree, &child_tvi) && child_tvi.lParam != kPlaceholderParam) {
            return; // real children already inserted, e.g. a re-collapse/expand
        }
    }

    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    // Remove the placeholder before inserting the real level.
    HTREEITEM child = TreeView_GetChild(tree, item);
    while (child != nullptr) {
        HTREEITEM next = TreeView_GetNextSibling(tree, child);
        TreeView_DeleteItem(tree, child);
        child = next;
    }
    // Depth passed as 0 here: kMaxInsertDepth now bounds how many *expand*
    // actions deep a user can drill (each expand costs one level), not a
    // single recursive walk, so this is a generous ceiling in practice.
    insert_one_level(tree, item, *node, 0);
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree, nullptr, TRUE);
}

} // namespace

void register_class(HINSTANCE) {
    // SysTreeView32 is a stock common control (loaded via InitCommonControlsEx
    // elsewhere at startup); nothing to register here.
}

HWND create(HWND parent, HINSTANCE instance, int control_id, int x, int y, int width, int height) {
    HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES | TVS_HASBUTTONS |
                                     TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                                 x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
                                 instance, nullptr);
    if (hwnd != nullptr) {
        static HFONT tree_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                              FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(tree_font), TRUE);
    }
    return hwnd;
}

void set_tree(HWND hwnd, const ParsedNodePtr& root) {
    if (hwnd == nullptr) {
        return;
    }
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(hwnd);

    // Keep the new root alive for as long as the tree is showing it -- every
    // lParam the control holds after this point points into it.
    s_roots[hwnd] = root;

    if (root != nullptr) {
        std::wstring label = format_label(*root);
        TVINSERTSTRUCTW is{};
        is.hParent = TVI_ROOT;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        is.item.pszText = const_cast<wchar_t*>(label.c_str());
        is.item.lParam = reinterpret_cast<LPARAM>(root.get());
        is.item.cChildren = root->children.empty() ? 0 : 1;
        HTREEITEM top = TreeView_InsertItem(hwnd, &is);

        // Only the root's immediate children (the "categories" -- PF header,
        // BIDX chunk, language slots, etc) are materialized up front. Each
        // of those gets a placeholder if it has its own children, so opening
        // a huge entry (tens of thousands of leaves several levels down)
        // costs one cheap level of TreeView_InsertItem calls, not a full
        // recursive walk -- the rest is filled in on demand as the user
        // actually expands a category (see handle_notify/expand_node).
        if (!root->children.empty()) {
            insert_one_level(hwnd, top, *root, 0);
        }
        TreeView_Expand(hwnd, top, TVE_EXPAND);
        // Categories themselves start collapsed: the user picks which one to
        // drill into instead of every language slot / chunk / array paying
        // to populate (and render) at once. This is the behavior change from
        // before -- previously every immediate child was force-expanded here
        // too, which is exactly what walked tens of thousands of fileId
        // leaves per language slot on a single selection and froze the UI.
    }

    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT handle_notify(HWND hwnd, NMHDR* header) {
    if (header == nullptr || header->code != TVN_ITEMEXPANDING) {
        return 0;
    }
    auto* nm = reinterpret_cast<NMTREEVIEWW*>(header);
    if (nm->action == TVE_EXPAND) {
        expand_node(hwnd, nm->itemNew.hItem);
    }
    return 0; // allow the expand/collapse to proceed
}

void set_message(HWND hwnd, const std::wstring& message) {
    if (hwnd == nullptr) {
        return;
    }
    TreeView_DeleteAllItems(hwnd);
    s_roots[hwnd] = nullptr;
    TVINSERTSTRUCTW is{};
    is.hParent = TVI_ROOT;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT;
    is.item.pszText = const_cast<wchar_t*>(message.c_str());
    TreeView_InsertItem(hwnd, &is);
}

void clear(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    TreeView_DeleteAllItems(hwnd);
    s_roots[hwnd] = nullptr;
}

} // namespace castlemist::structtree
