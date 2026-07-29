/// @file
/// @brief Small helpers for the report-view ListView tables.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>

namespace castlemist::ui {

// --- Small helpers for the cntc content-browser sortable ListView tables. ---
// Append a report-view column.
void lv_add_col(HWND lv, int i, const wchar_t* text, int width) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = const_cast<wchar_t*>(text);
    c.cx = width;
    c.iSubItem = i;
    ListView_InsertColumn(lv, i, &c);
}
// Append a row: col0 text + lParam (a stable index into the backing vector, so
// sorting the visible rows never loses the mapping); extra column texts follow.
int lv_add_row(HWND lv, LPARAM param, const wchar_t* col0,
               const wchar_t* col1, const wchar_t* col2,
               const wchar_t* col3, const wchar_t* col4) {
    LVITEMW it{};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = ListView_GetItemCount(lv);
    it.iSubItem = 0;
    it.pszText = const_cast<wchar_t*>(col0);
    it.lParam = param;
    int row = static_cast<int>(ListView_InsertItem(lv, &it));
    if (col1) ListView_SetItemText(lv, row, 1, const_cast<wchar_t*>(col1));
    if (col2) ListView_SetItemText(lv, row, 2, const_cast<wchar_t*>(col2));
    if (col3) ListView_SetItemText(lv, row, 3, const_cast<wchar_t*>(col3));
    if (col4) ListView_SetItemText(lv, row, 4, const_cast<wchar_t*>(col4));
    return row;
}
// lParam of the selected row (the backing index), or -1 if nothing is selected.
LPARAM lv_selected_param(HWND lv) {
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) return -1;
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    ListView_GetItem(lv, &it);
    return it.lParam;
}

void layout_video_subtitle(); // defined with the other video helpers, below

} // namespace castlemist::ui
