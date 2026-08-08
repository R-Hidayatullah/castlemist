#ifndef GW2_STRUCT_TREE_H
#define GW2_STRUCT_TREE_H

#include <windows.h>

#include <string>

#include "castlemist/native/ParsedNode.h"

/// A native SysTreeView32-backed panel that shows the currently selected
/// entry's decompressed bytes walked according to the loaded JSON struct
/// template (`castlemist::tpl::get()`, see struct_template.h) -- i.e. the
/// same ParsedNode tree BinaryParser already builds for the model/map
/// extractors, just rendered directly instead of being consumed internally.
///
/// Sits as its own "Structure" tab next to Compressed / Decompressed / Preview,
/// immediately after the hex-viewer panels: those two show raw bytes, this one
/// shows the same bytes decoded into a category tree (PF header -> chunks ->
/// fields), one node per struct/array/field, expandable like any file/registry
/// tree. Offset + size + decoded value are shown inline per node so it reads
/// like a structured alternative to scrolling the hex view by hand.
namespace castlemist::structtree {

/// Registers nothing of its own (SysTreeView32 is a stock common control) --
/// present for symmetry with the other panel factories and to keep the
/// call site in window_proc.cpp uniform.
void register_class(HINSTANCE instance);

/// Creates the tree view child control.
HWND create(HWND parent, HINSTANCE instance, int control_id, int x, int y, int width, int height);

/// Populates the tree from an already-parsed node tree (see BinaryParser::parse).
/// Replaces any previous contents. A null root clears the tree.
void set_tree(HWND hwnd, const ParsedNodePtr& root);

/// Shows a single-line informational/error message instead of a tree -- used
/// when there is no struct template loaded, no template matches this entry's
/// chunk, or the parse failed outright.
void set_message(HWND hwnd, const std::wstring& message);

/// Clears the tree back to empty (no message, no nodes).
void clear(HWND hwnd);

} // namespace castlemist::structtree

#endif // GW2_STRUCT_TREE_H
