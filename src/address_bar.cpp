#include "address_bar.h"
#include "app.h"
#include "shell_view.h"
#include "ui_icons.h"
namespace zen {
AddressBar::~AddressBar() {
    ReleaseCompletion();
    if (edit) {
        RemoveWindowSubclass(edit, EditProc, 0);
        DestroyWindow(edit);
    }
    if (crumb)
        DestroyWindow(crumb);
}
void AddressBar::ReleaseCompletion() {
    if (complete)
        complete->Enable(FALSE);
    dropDown.Reset();
    complete.Reset();
}
HRESULT AddressBar::Create(HWND first, HWND last) {
    if (pane < 0 || pane > 1 || !IsWindow(app.window) || !IsWindow(first) || !IsWindow(last))
        return E_INVALIDARG;
    if (edit || crumb)
        return E_UNEXPECTED;
    firstNav = first;
    lastNav = last;
    WNDCLASSW wc{};
    wc.hInstance = app.instance;
    wc.lpfnWndProc = CrumbProc;
    wc.lpszClassName = L"ZenCrumb";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(GetLastError());
    crumb = CreateWindowW(L"ZenCrumb", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 1, 1, app.window,
                          nullptr, app.instance, this);
    if (!crumb)
        return E_FAIL;
    edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 1, 1, app.window, nullptr,
                           app.instance, nullptr);
    if (!edit)
        return E_FAIL;
    if (!SetWindowSubclass(edit, EditProc, 0, (DWORD_PTR)this)) {
        DestroyWindow(edit);
        edit = nullptr;
        return E_FAIL;
    }
    SendMessageW(edit, WM_SETFONT, (WPARAM)app.font, TRUE);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    ComPtr<IACList2> paths;
    HRESULT completeHr =
        CoCreateInstance(CLSID_ACListISF, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&paths));
    if (SUCCEEDED(completeHr))
        completeHr = paths->SetOptions(ACLO_CURRENTDIR | ACLO_MYCOMPUTER | ACLO_FILESYSDIRS);
    if (SUCCEEDED(completeHr))
        completeHr =
            CoCreateInstance(CLSID_AutoComplete, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&complete));
    if (SUCCEEDED(completeHr))
        completeHr = complete->SetOptions(ACO_AUTOSUGGEST | ACO_AUTOAPPEND | ACO_UPDOWNKEYDROPSLIST);
    if (SUCCEEDED(completeHr))
        completeHr = complete->Init(edit, paths.Get(), nullptr, nullptr);
    if (SUCCEEDED(completeHr))
        completeHr = complete.As(&dropDown);
    if (FAILED(completeHr)) {
        Log(L"地址栏系统补全初始化失败 " + ErrorText(completeHr));
        if (complete)
            complete->Enable(FALSE);
        dropDown.Reset();
        complete.Reset();
    }
    return S_OK;
}
// 各子控件绘制同一条整栏底线的可见部分，避免按钮重绘覆盖高亮。
void AddressBar::PaintBorder(HDC dc, HWND child) const {
    RECT first{}, last{};
    GetWindowRect(firstNav, &first);
    GetWindowRect(lastNav, &last);
    POINT corners[]{{first.left, first.top}, {last.right, last.bottom}};
    MapWindowPoints(nullptr, child, corners, 2);
    HPEN pen =
        CreatePen(PS_SOLID, app.S(app.model.active == pane ? 2 : 1),
                  app.model.active == pane ? (app.windowActive ? RGB(55, 125, 220) : RGB(143, 165, 194))
                                           : RGB(190, 190, 190));
    auto old = SelectObject(dc, pen);
    POINT border[]{{corners[0].x, corners[1].y - 1}, {corners[1].x, corners[1].y - 1}};
    Polyline(dc, border, ARRAYSIZE(border));
    SelectObject(dc, old);
    DeleteObject(pen);
}
LRESULT CALLBACK AddressBar::EditProc(HWND w, UINT m, WPARAM a, LPARAM b, UINT_PTR index, DWORD_PTR ptr) {
    auto *bar = reinterpret_cast<AddressBar *>(ptr);
    auto *app = &bar->app;
    int p = bar->pane;
    auto candidatesVisible = [&] {
        DWORD flags = 0;
        return bar->dropDown && SUCCEEDED(bar->dropDown->GetDropDownStatus(&flags, nullptr)) &&
               (flags & ACDD_VISIBLE);
    };
    if (m == WM_CHAR && (a == VK_RETURN || a == VK_ESCAPE))
        return 0;
    if (m == WM_KEYDOWN) {
        if ((a == VK_RETURN || a == VK_ESCAPE) && candidatesVisible()) {
            LPWSTR selected = nullptr;
            if (a == VK_RETURN)
                bar->dropDown->GetDropDownStatus(nullptr, &selected);
            bar->complete->Enable(FALSE);
            if (selected && *selected) {
                SetWindowTextW(w, selected);
                SendMessageW(w, EM_SETSEL, (WPARAM)-1, -1);
            }
            CoTaskMemFree(selected);
            bar->complete->Enable(TRUE);
            return 0;
        }
        if (a == VK_RETURN) {
            auto text = WindowText(w);
            ShowWindow(w, SW_HIDE);
            ShowWindow(bar->crumb, SW_SHOW);
            const auto &session = bar->session;
            auto *tab = app->model.Find(app->model.panes[p].selected);
            if (tab && tab->id == session.tab && tab->path == session.path && text == session.text)
                app->Activate(p, 0, true);
            else
                app->Navigate(app->model.panes[p].selected, text.empty() ? Home : text);
            return 0;
        }
        if (a == VK_ESCAPE) {
            ShowWindow(w, SW_HIDE);
            ShowWindow(bar->crumb, SW_SHOW);
            app->Activate(p, 0, true);
            return 0;
        }
    }
    if (m == WM_KILLFOCUS)
        PostMessageW(w, WM_APP + 101, 0, 0);
    if (m == WM_APP + 101 && GetFocus() != w && !candidatesVisible()) {
        // 延迟到系统候选列表处理失焦后再判断，避免点击候选时提前取消。
        SetWindowTextW(w, app->AddressText(p).c_str());
        ShowWindow(w, SW_HIDE);
        ShowWindow(bar->crumb, SW_SHOW);
    }
    if (m == WM_SETFOCUS)
        app->Activate(p);
    if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(w, EditProc, index);
        bar->edit = nullptr;
        bar->ReleaseCompletion();
    }
    return DefSubclassProc(w, m, a, b);
}
void AddressBar::ShowFolders(HWND owner, Id tab, const Link &link) {
    std::vector<ComPtr<IShellItem>> folders;
    ComPtr<IShellItem> parent;
    HRESULT hr = app.model.locations.ResolveItem(link.path, &parent);
    if (SUCCEEDED(hr))
        hr = ChildFolderItems(parent.Get(), folders);
    if (FAILED(hr)) {
        Report(app.window, L"无法读取子文件夹。", hr);
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    for (size_t i = 0; i < folders.size(); ++i) {
        std::wstring label;
        for (wchar_t c : ItemName(folders[i].Get(), SIGDN_NORMALDISPLAY)) {
            label += c;
            if (c == L'&')
                label += c;
        }
        AppendMenuW(menu, MF_STRING, i + 1, label.c_str());
    }
    if (folders.empty())
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"无子文件夹");
    POINT point{link.rect.left, link.rect.bottom};
    ClientToScreen(owner, &point);
    UINT chosen = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, point.x,
                                   point.y, app.window, nullptr);
    DestroyMenu(menu);
    if (chosen && chosen <= folders.size() && app.model.panes[pane].selected == tab && app.model.Find(tab)) {
        app.Activate(pane);
        auto path = app.RememberLocation(folders[chosen - 1].Get());
        if (!path.empty()) {
            app.Navigate(tab, path);
        }
    }
}
LRESULT CALLBACK AddressBar::CrumbProc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto *d = (AddressBar *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        d = (AddressBar *)((CREATESTRUCTW *)b)->lpCreateParams;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)d);
    }
    if (!d)
        return DefWindowProcW(w, m, a, b);
    auto &app = d->app;
    int p = d->pane;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        auto old = SelectObject(dc, app.font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        d->links.clear();
        auto *t = app.model.Find(app.model.panes[p].selected);
        int x = app.S(8);
        if (t) {
            auto view = app.views.find(t->id);
            auto parts = (GetWindowLongPtrW(d->edit, GWL_STYLE) & WS_VISIBLE)
                             ? std::vector<std::pair<std::wstring, LocationKey>>{}
                         : view != app.views.end() ? view->second->AddressParts()
                                                   : app.model.locations.FallbackParts(t->path);
            DrawIconEx(dc, x, (rc.bottom - app.S(16)) / 2, app.Icon(t->id), app.S(16), app.S(16), 0, nullptr,
                       DI_NORMAL);
            x += app.S(26);
            for (auto &[label, path2] : parts) {
                SIZE size{};
                GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &size);
                const int arrowWidth = app.S(18), padding = app.S(6);
                int width = std::min((int)size.cx + padding * 2, app.S(220)) + arrowWidth;
                RECT r{x, app.S(3), std::min(x + width, (int)rc.right - app.S(5)), rc.bottom - app.S(3)};
                if (r.right - r.left < arrowWidth + padding * 2)
                    break;
                RECT arrow{r.right - arrowWidth, r.top, r.right, r.bottom};
                if (PtInRect(&r, d->hover)) {
                    auto brush = CreateSolidBrush(RGB(232, 240, 250));
                    FillRect(dc, &r, brush);
                    DeleteObject(brush);
                    RECT separator{arrow.left, r.top + app.S(3), arrow.left + app.S(1), r.bottom - app.S(3)};
                    brush = CreateSolidBrush(RGB(190, 207, 225));
                    FillRect(dc, &separator, brush);
                    DeleteObject(brush);
                }
                RECT name{r.left + padding, r.top, arrow.left - padding, r.bottom};
                DrawTextW(dc, label.c_str(), -1, &name,
                          DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                DrawUiIcon(dc, arrow, UiIcon::ChevronRight, app.dpi);
                d->links.push_back({r, arrow, path2});
                x += width;
            }
        }
        d->PaintBorder(dc, w);
        // 分隔导航按钮与地址内容；编辑框留在两条分隔线内侧。
        HBRUSH divider = CreateSolidBrush(RGB(205, 205, 205));
        RECT leftDivider{0, app.S(8), app.S(1), rc.bottom - app.S(8)};
        RECT rightDivider{rc.right - app.S(1), app.S(8), rc.right, rc.bottom - app.S(8)};
        FillRect(dc, &leftDivider, divider);
        FillRect(dc, &rightDivider, divider);
        DeleteObject(divider);
        SelectObject(dc, old);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        POINT next{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        bool changed = false;
        for (const auto &[rect, arrow, path] : d->links)
            if (PtInRect(&rect, d->hover) != PtInRect(&rect, next))
                changed = true;
        d->hover = next;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, w, 0};
        TrackMouseEvent(&track);
        if (changed)
            InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    if (m == WM_MOUSELEAVE || m == WM_SHOWWINDOW) {
        d->hover = {-1, -1};
        InvalidateRect(w, nullptr, FALSE);
        if (m == WM_MOUSELEAVE)
            return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        app.Activate(p);
        POINT pt{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        for (const auto &entry : d->links)
            if (PtInRect(&entry.rect, pt)) {
                // 弹出菜单会重入绘制，先复制命中信息，避免持有 links 内部引用。
                auto link = entry;
                Id tab = app.model.panes[p].selected;
                if (PtInRect(&link.arrow, pt))
                    d->ShowFolders(w, tab, link);
                else
                    app.Navigate(tab, link.path);
                return 0;
            }
        app.EditAddress(p);
        return 0;
    }
    if (m == WM_NCDESTROY) {
        d->crumb = nullptr;
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(w, m, a, b);
}
std::wstring AddressBar::Text() const {
    const int p = pane;
    auto &model = app.model;
    auto &views = app.views;
    auto *tab = model.Find(model.panes[p].selected);
    if (!tab || tab->path == Home)
        return {};
    auto view = views.find(tab->id);
    return view != views.end() ? view->second->AddressEditText() : model.locations.Text(tab->path);
}
void AddressBar::Update() {
    if (!edit || !crumb)
        return;
    const int p = pane;
    auto &model = app.model;
    auto *t = model.Find(model.panes[p].selected);
    if (!t)
        return;
    if (GetFocus() != edit)
        SetWindowTextW(edit, Text().c_str());
    InvalidateRect(crumb, nullptr, FALSE);
}
void AddressBar::Edit() {
    if (!edit || !crumb)
        return;
    const int p = pane;
    auto &model = app.model;
    if (GetWindowLongPtrW(edit, GWL_STYLE) & WS_VISIBLE) {
        SetFocus(edit);
        return;
    }
    app.Activate(p);
    Update();
    // 隐藏的编辑框可能仍保留焦点；开始新编辑会话时始终取当前标签文字。
    SetWindowTextW(edit, Text().c_str());
    if (auto *tab = model.Find(model.panes[p].selected))
        session = {tab->id, tab->path, WindowText(edit)};
    ShowWindow(edit, SW_SHOW);
    SetWindowPos(edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(crumb, nullptr, FALSE);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
}
} // namespace zen
