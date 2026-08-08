#include "castlemist/ui/struct_tree.h"

#include <commctrl.h>

#include <cstdio>
#include <cwchar>

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
// side, but a template can still legitimately nest a few hundred chunks x
// fields deep for a big packfile; capping the *tree build* separately keeps a
// pathological file from turning "select an entry" into a multi-second stall
// insertion loop rather than truncating output the user asked to see.
constexpr int kMaxInsertDepth = 64;
constexpr size_t kMaxChildrenPerNode = 20000;

void insert_children(HWND tree, HTREEITEM parent, const ParsedNode& node, int depth) {
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
        is.item.mask = TVIF_TEXT | TVIF_PARAM;
        is.item.pszText = const_cast<wchar_t*>(label.c_str());
        // Stash the offset so a future "sync with hex view" click handler has
        // somewhere to read it from without re-walking the tree.
        is.item.lParam = static_cast<LPARAM>(child->offset);
        HTREEITEM item = TreeView_InsertItem(tree, &is);

        if (!child->children.empty()) {
            insert_children(tree, item, *child, depth + 1);
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

    if (root != nullptr) {
        std::wstring label = format_label(*root);
        TVINSERTSTRUCTW is{};
        is.hParent = TVI_ROOT;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM;
        is.item.pszText = const_cast<wchar_t*>(label.c_str());
        is.item.lParam = static_cast<LPARAM>(root->offset);
        HTREEITEM top = TreeView_InsertItem(hwnd, &is);

        insert_children(hwnd, top, *root, 0);
        TreeView_Expand(hwnd, top, TVE_EXPAND);
        // One level below the root (e.g. every chunk) starts expanded too --
        // that is the "category tree" the caller asked for: chunks are the
        // categories, their fields are what you drill into.
        HTREEITEM child = TreeView_GetChild(hwnd, top);
        while (child != nullptr) {
            TreeView_Expand(hwnd, child, TVE_EXPAND);
            child = TreeView_GetNextSibling(hwnd, child);
        }
    }

    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void set_message(HWND hwnd, const std::wstring& message) {
    if (hwnd == nullptr) {
        return;
    }
    TreeView_DeleteAllItems(hwnd);
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
}

} // namespace castlemist::structtree
