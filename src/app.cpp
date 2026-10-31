#include "app.h"
#include "operations.h"
#include "shell_view.h"
#include "shell_menu.h"
#include "tabs.h"
#include "ui_icons.h"
#include <commdlg.h>
#include <set>
#include <gdiplus.h>
namespace zen {
static constexpr UINT Back = 101, Forward = 102, Up = 103, Refresh = 104, Exit = 110, ClearRecent = 111,
                      Wrap = 112, About = 113, SingleGroup = 114, ViewBase = 200, RecentBase = 300;
static LRESULT CALLBACK AddressProc(HWND w, UINT m, WPARAM a, LPARAM b, UINT_PTR index, DWORD_PTR ptr) {
    auto *app = (App *)ptr;
    int p = (int)index;
    auto candidatesVisible = [&] {
        DWORD flags = 0;
        return app->addressDropDown[p] &&
               SUCCEEDED(app->addressDropDown[p]->GetDropDownStatus(&flags, nullptr)) &&
               (flags & ACDD_VISIBLE);
    };
    if (m == WM_CHAR && (a == VK_RETURN || a == VK_ESCAPE))
        return 0;
    if (m == WM_KEYDOWN) {
        if ((a == VK_RETURN || a == VK_ESCAPE) && candidatesVisible()) {
            LPWSTR selected = nullptr;
            if (a == VK_RETURN)
                app->addressDropDown[p]->GetDropDownStatus(nullptr, &selected);
            app->addressComplete[p]->Enable(FALSE);
            if (selected && *selected) {
                SetWindowTextW(w, selected);
                SendMessageW(w, EM_SETSEL, (WPARAM)-1, -1);
            }
            CoTaskMemFree(selected);
            app->addressComplete[p]->Enable(TRUE);
            return 0;
        }
        if (a == VK_RETURN) {
            auto text = WindowText(w);
            ShowWindow(w, SW_HIDE);
            ShowWindow(app->crumb[p], SW_SHOW);
            if (!text.empty())
                app->Navigate(app->model.panes[p].selected, text);
            return 0;
        }
        if (a == VK_ESCAPE) {
            ShowWindow(w, SW_HIDE);
            ShowWindow(app->crumb[p], SW_SHOW);
            app->Activate(p, 0, true);
            return 0;
        }
    }
    if (m == WM_KILLFOCUS)
        PostMessageW(w, WM_APP + 101, 0, 0);
    if (m == WM_APP + 101 && GetFocus() != w && !candidatesVisible()) {
        // 延迟到系统候选列表处理失焦后再判断，避免点击候选时提前取消。
        if (auto *tab = app->model.Find(app->model.panes[p].selected))
            SetWindowTextW(w, tab->path.c_str());
        ShowWindow(w, SW_HIDE);
        ShowWindow(app->crumb[p], SW_SHOW);
    }
    if (m == WM_SETFOCUS)
        app->Activate(p);
    return DefSubclassProc(w, m, a, b);
}
struct CrumbData {
    App *app;
    int pane;
    POINT hover{-1, -1};
    std::vector<std::pair<RECT, std::wstring>> links;
};
static LRESULT CALLBACK CrumbProc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto *d = (CrumbData *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        d = (CrumbData *)((CREATESTRUCTW *)b)->lpCreateParams;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)d);
    }
    if (!d)
        return DefWindowProcW(w, m, a, b);
    auto &app = *d->app;
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
            auto parts = (GetWindowLongPtrW(app.address[p], GWL_STYLE) & WS_VISIBLE)
                             ? std::vector<std::pair<std::wstring, std::wstring>>{}
                         : view != app.views.end() ? view->second->AddressParts()
                                                   : Breadcrumbs(t->path);
            DrawIconEx(dc, x, (rc.bottom - app.S(16)) / 2, app.Icon(t->id), app.S(16), app.S(16), 0, nullptr,
                       DI_NORMAL);
            x += app.S(26);
            std::vector<std::pair<RECT, std::wstring>> labels;
            for (auto &[label, path2] : parts) {
                SIZE size{};
                GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &size);
                int width = std::min((int)size.cx + app.S(24), app.S(220));
                RECT r{x, app.S(3), std::min(x + width, (int)rc.right - app.S(5)), rc.bottom - app.S(3)};
                if (r.right <= r.left)
                    break;
                // 只移动响应背景，不改变文本布局、层级步长或截断位置。
                RECT hit = r;
                OffsetRect(&hit, -app.S(6), 0);
                if (PtInRect(&hit, d->hover)) {
                    auto brush = CreateSolidBrush(RGB(232, 240, 250));
                    FillRect(dc, &hit, brush);
                    DeleteObject(brush);
                }
                labels.push_back({r, label + L" ›"});
                d->links.push_back({hit, path2});
                x += width;
            }
            // 统一在背景之后绘制，避免相邻背景盖住长名称末尾的省略号。
            for (auto &[rect, text] : labels)
                DrawTextW(dc, text.c_str(), -1, &rect,
                          DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        HPEN pen = CreatePen(PS_SOLID, app.S(app.model.active == p ? 2 : 1),
                             app.model.active == p ? RGB(55, 125, 220) : RGB(190, 190, 190));
        auto op = SelectObject(dc, pen);
        auto ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, rc.right, rc.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
        SelectObject(dc, old);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        POINT next{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        bool changed = false;
        for (const auto &[rect, path] : d->links)
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
        for (auto &[r, path] : d->links)
            if (PtInRect(&r, pt)) {
                app.Navigate(app.model.panes[p].selected, path);
                return 0;
            }
        app.EditAddress(p);
        return 0;
    }
    if (m == WM_NCDESTROY) {
        delete d;
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(w, m, a, b);
}
App::App() = default;
App::~App() {
    views.clear();
    strips[0].reset();
    strips[1].reset();
    for (auto &[id, icon] : icons)
        if (icon)
            DestroyIcon(icon);
    for (auto icon : {folderIcon, computerIcon, missingIcon, windowLargeIcon, windowSmallIcon})
        if (icon)
            DestroyIcon(icon);
    if (font)
        DeleteObject(font);
}
int App::Run(HINSTANCE h, int show, const std::wstring &path) {
    instance = h;
    std::error_code ec;
    std::filesystem::create_directories(ConfigDir(), ec);
    bool recovered = false, future = false;
    LoadModel(model, ConfigDir() / L"session.dat", recovered, future);
    readOnly = future;
    // 恢复后确保当前标签可见。
    for (int p = 0; p < 2; ++p)
        for (const auto &group : model.panes[p].groups)
            if (group.collapsed)
                model.SetGroupCollapsed(p, group.id, true);
    // 测试配置目录具有独立实例标识，避免干扰用户正常会话。
    auto dir = ConfigDir().wstring();
    uint64_t hash = 1469598103934665603ull;
    for (auto c : dir)
        hash = (hash ^ c) * 1099511628211ull;
    std::wstring key = L"Local\\ZenExplorer-" + std::to_wstring(hash);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, key.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW((L"ZenExplorerWindow-" + std::to_wstring(hash)).c_str(), nullptr);
        if (existing) {
            if (!path.empty()) {
                COPYDATASTRUCT data{1, (DWORD)((path.size() + 1) * 2), (void *)path.c_str()};
                DWORD_PTR result;
                SendMessageTimeoutW(existing, WM_COPYDATA, 0, (LPARAM)&data, SMTO_ABORTIFHUNG, 2000, &result);
            }
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        } else
            MessageBoxW(nullptr, L"已有实例正在启动或结束，请稍后重试。", L"ZenExplorer", MB_OK);
        CloseHandle(mutex);
        return 0;
    }
    WNDCLASSW wc{};
    wc.hInstance = h;
    wc.lpfnWndProc = Proc;
    std::wstring cls = L"ZenExplorerWindow-" + std::to_wstring(hash);
    wc.lpszClassName = cls.c_str();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(h, MAKEINTRESOURCEW(101));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    RECT r = model.window;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &monitor);
    int width = std::clamp(r.right - r.left, 640L, monitor.rcWork.right - monitor.rcWork.left);
    int height = std::clamp(r.bottom - r.top, 400L, monitor.rcWork.bottom - monitor.rcWork.top);
    r.left = std::clamp(r.left, monitor.rcWork.left, monitor.rcWork.right - width);
    r.top = std::clamp(r.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
    window = CreateWindowExW(0, cls.c_str(), L"ZenExplorer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, r.left,
                             r.top, width, height, nullptr, nullptr, h, this);
    if (!window) {
        CloseHandle(mutex);
        return 2;
    }
    {
        int savedWidth = model.window.right - model.window.left;
        int savedHeight = model.window.bottom - model.window.top;
        if (model.windowDpi) {
            savedWidth = MulDiv(savedWidth, dpi, model.windowDpi);
            savedHeight = MulDiv(savedHeight, dpi, model.windowDpi);
        }
        int workWidth = monitor.rcWork.right - monitor.rcWork.left;
        int workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
        width = std::clamp(savedWidth, std::min(S(640), workWidth), workWidth);
        height = std::clamp(savedHeight, std::min(S(400), workHeight), workHeight);
        r.left = std::clamp(r.left, monitor.rcWork.left, monitor.rcWork.right - width);
        r.top = std::clamp(r.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
        SetWindowPos(window, nullptr, r.left, r.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(window, model.maximized ? SW_SHOWMAXIMIZED : show);
    RefreshWindowIcons();
    UpdateWindow(window);
    if (recovered) {
        Log(L"从有效备份恢复会话");
        MessageBoxW(window, L"会话文件异常，已从备份恢复。", L"ZenExplorer",
                    MB_OK | MB_ICONINFORMATION); // 避免下一次保存用损坏主文件覆盖有效备份。
        auto corrupt = ConfigDir() / L"session.corrupt";
        MoveFileExW((ConfigDir() / L"session.dat").c_str(), corrupt.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (future)
        MessageBoxW(window, L"配置来自较新版本。本次使用默认会话且不会覆盖原配置。", L"ZenExplorer",
                    MB_OK | MB_ICONWARNING);
    if (!path.empty())
        NewTab(model.active, path);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ViewMouse(msg);
        if (!Key(msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CloseHandle(mutex);
    return (int)msg.wParam;
}
LRESULT CALLBACK App::Proc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto *app = (App *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        app = (App *)((CREATESTRUCTW *)b)->lpCreateParams;
        app->window = w;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)app);
    }
    return app ? app->Message(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT App::Message(UINT m, WPARAM a, LPARAM b) {
    if ((m == WM_ACTIVATEAPP && !a) || m == WM_ENTERMENULOOP)
        ++pasteGeneration;
    if (menu3 && (m == WM_INITMENUPOPUP || m == WM_DRAWITEM || m == WM_MEASUREITEM || m == WM_MENUCHAR)) {
        LRESULT result = 0;
        if (SUCCEEDED(menu3->HandleMenuMsg2(m, a, b, &result)))
            return result;
    } else if (menu2 && (m == WM_INITMENUPOPUP || m == WM_DRAWITEM || m == WM_MEASUREITEM))
        if (SUCCEEDED(menu2->HandleMenuMsg(m, a, b)))
            return 0;
    switch (m) {
    case WM_DRAWITEM: {
        auto *d = (DRAWITEMSTRUCT *)b;
        if (d->CtlType != ODT_BUTTON || d->CtlID < Back || d->CtlID > Refresh)
            break;
        RECT r = d->rcItem;
        bool disabled = (d->itemState & ODS_DISABLED) != 0;
        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(d->hwndItem, &cursor);
        COLORREF bg = (d->itemState & ODS_SELECTED)       ? RGB(210, 220, 233)
                      : PtInRect(&r, cursor) && !disabled ? RGB(228, 234, 242)
                                                          : GetSysColor(COLOR_BTNFACE);
        auto brush = CreateSolidBrush(bg);
        FillRect(d->hDC, &r, brush);
        DeleteObject(brush);
        DrawUiIcon(d->hDC, r, static_cast<UiIcon>(d->CtlID - Back), dpi, disabled);
        if (d->itemState & ODS_FOCUS) {
            InflateRect(&r, -S(3), -S(3));
            DrawFocusRect(d->hDC, &r);
        }
        return TRUE;
    }
    case WM_CREATE: {
        dpi = GetDpiForWindow(window);
        RefreshDpiResources();
        BuildMenu();
        for (int i = 0; i < 4; ++i) {
            const wchar_t *labels[] = {L"后退", L"前进", L"上一级", L"刷新"};
            HWND btn = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 1, 1,
                                     window, (HMENU)(INT_PTR)(Back + i), instance, nullptr);
            SendMessageW(btn, WM_SETFONT, (WPARAM)font, TRUE);
        }
        HWND tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0,
                                    0, 0, window, nullptr, instance, nullptr);
        const wchar_t *help[] = {L"后退（Alt+←）", L"前进（Alt+→）", L"上一级（Alt+↑）", L"刷新（F5）"};
        for (int i = 0; i < 4; ++i) {
            TOOLINFOW ti{sizeof(ti)};
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = window;
            ti.uId = (UINT_PTR)GetDlgItem(window, Back + i);
            ti.lpszText = const_cast<wchar_t *>(help[i]);
            SendMessageW(tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpfnWndProc = CrumbProc;
        wc.lpszClassName = L"ZenCrumb";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        for (int p = 0; p < 2; ++p) {
            strips[p] = std::make_unique<TabStrip>(*this, p);
            strips[p]->Create();
            crumb[p] = CreateWindowW(L"ZenCrumb", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                                     window, nullptr, instance, (new CrumbData{this, p}));
            address[p] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 1, 1, window,
                                         nullptr, instance, nullptr);
            SendMessageW(address[p], WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(address[p], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            ComPtr<IACList2> paths;
            HRESULT completeHr =
                CoCreateInstance(CLSID_ACListISF, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&paths));
            if (SUCCEEDED(completeHr))
                completeHr = paths->SetOptions(ACLO_CURRENTDIR | ACLO_MYCOMPUTER | ACLO_FILESYSDIRS);
            if (SUCCEEDED(completeHr))
                completeHr = CoCreateInstance(CLSID_AutoComplete, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&addressComplete[p]));
            if (SUCCEEDED(completeHr))
                completeHr =
                    addressComplete[p]->SetOptions(ACO_AUTOSUGGEST | ACO_AUTOAPPEND | ACO_UPDOWNKEYDROPSLIST);
            if (SUCCEEDED(completeHr))
                completeHr = addressComplete[p]->Init(address[p], paths.Get(), nullptr, nullptr);
            if (SUCCEEDED(completeHr))
                completeHr = addressComplete[p].As(&addressDropDown[p]);
            if (FAILED(completeHr)) {
                Log(L"地址栏系统补全初始化失败 " + ErrorText(completeHr));
                if (addressComplete[p])
                    addressComplete[p]->Enable(FALSE);
                addressDropDown[p].Reset();
                addressComplete[p].Reset();
            }
            SetWindowSubclass(address[p], AddressProc, p, (DWORD_PTR)this);
            error[p] = CreateWindowW(L"BUTTON", L"目录无法访问，点击重试", WS_CHILD | BS_MULTILINE, 0, 0, 1,
                                     1, window, (HMENU)(INT_PTR)(500 + p), instance, nullptr);
            SendMessageW(error[p], WM_SETFONT, (WPARAM)font, TRUE);
        }
        status = CreateWindowW(STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window, nullptr,
                               instance, nullptr);
        SendMessageW(status, WM_SETFONT, (WPARAM)font, TRUE);
        SetTimer(window, 1, 700, nullptr);
        PostMessageW(window, WM_ZEN_ACTION, 1, 0);
        return 0;
    }
    case WM_ZEN_ACTION:
        if (a == 1) {
            for (auto &pane : model.panes)
                for (auto &tab : pane.tabs)
                    RefreshLocalIcon(tab.id);
            EnsureView(model.panes[0].selected);
            EnsureView(model.panes[1].selected);
            Layout();
        }
        return 0;
    case WM_ZEN_NAV: {
        std::unique_ptr<NavRequest> r((NavRequest *)b);
        if (model.Find(r->id)) {
            if (r->newTab) {
                int p = model.PaneOf(r->id);
                auto *t = model.Find(r->id);
                NewTab(p, r->path, t->group, model.Index(p, t->id) + 1, r->id);
            } else
                Navigate(r->id, r->path);
        }
        return 0;
    }
    case WM_ZEN_CHANGED: {
        auto it = views.find((Id)a);
        if (it != views.end()) {
            it->second->Hook();
            if (b == 1)
                ShowError((Id)a, it->second->error);
        }
        Changed();
        if (b != 3) {
            Layout();
        }
        UpdateStatus();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(a) == 500 || LOWORD(a) == 501) {
            int p = LOWORD(a) - 500;
            auto *t = model.Find(model.panes[p].selected);
            if (t)
                Navigate(t->id, t->path);
        } else
            Command(LOWORD(a));
        return 0;
    case WM_SIZE:
        Layout();
        Changed();
        return 0;
    case WM_MOVE:
        Changed();
        return 0;
    case WM_DPICHANGED: {
        dpi = HIWORD(a);
        RefreshWindowIcons();
        RefreshDpiResources();
        auto *r = (RECT *)b;
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT r;
        GetClientRect(window, &r);
        int x = (int)(r.right * model.split);
        if (abs(GET_X_LPARAM(b) - x) < S(6)) {
            sizing = true;
            SetCapture(window);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (sizing) {
            RECT r;
            GetClientRect(window, &r);
            model.split = std::clamp((double)GET_X_LPARAM(b) / std::max(1L, r.right), .15, .85);
            Layout();
            Changed();
        }
        return 0;
    case WM_LBUTTONUP:
        if (sizing) {
            sizing = false;
            ReleaseCapture();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(b) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(window, &p);
            RECT r;
            GetClientRect(window, &r);
            if (abs(p.x - (int)(r.right * model.split)) < S(5)) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    case WM_TIMER:
        if (!closing) {
            PollPastes();
            for (int p = 0; p < 2; ++p)
                if (GetWindowLongPtrW(address[p], GWL_STYLE) & WS_VISIBLE)
                    PostMessageW(address[p], WM_APP + 101, 0, 0);
            for (auto &[id, v] : views)
                v->Hook();
            if (dirty)
                Save();
        }
        return 0;
    case WM_COPYDATA: {
        auto *d = (COPYDATASTRUCT *)b;
        if (d && d->dwData == 1 && d->cbData >= 2 && d->cbData <= 65536 && d->cbData % 2 == 0) {
            auto *p = (wchar_t *)d->lpData;
            if (p[d->cbData / 2 - 1] == 0) {
                NewTab(model.active, p);
                return TRUE;
            }
        }
        return FALSE;
    }
    case WM_QUERYENDSESSION:
        Save();
        return TRUE;
    case WM_CLOSE:
        closing = true;
        Save();
        OleFlushClipboard();
        views.clear();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, m, a, b);
}
void App::RefreshWindowIcons() {
    // 任务栏和标题栏分别加载当前 DPI 对应帧，切换显示器时同步重载。
    auto replace = [&](HICON &owned, WPARAM type, int metricX, int metricY) {
        HICON next =
            (HICON)LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                              GetSystemMetricsForDpi(metricX, dpi), GetSystemMetricsForDpi(metricY, dpi), 0);
        if (!next)
            return;
        SendMessageW(window, WM_SETICON, type, (LPARAM)next);
        if (owned)
            DestroyIcon(owned);
        owned = next;
    };
    replace(windowLargeIcon, ICON_BIG, SM_CXICON, SM_CYICON);
    replace(windowSmallIcon, ICON_SMALL, SM_CXSMICON, SM_CYSMICON);
}
void App::RefreshDpiResources() {
    HFONT next = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    if (next) {
        // 先替换所有持有旧字体的自有控件，再释放旧字体；不接管 Shell 内部控件字体。
        for (HWND child : {address[0], address[1], error[0], error[1], status})
            if (child)
                SendMessageW(child, WM_SETFONT, (WPARAM)next, TRUE);
        for (int i = 0; i < 4; ++i)
            if (HWND button = GetDlgItem(window, Back + i))
                SendMessageW(button, WM_SETFONT, (WPARAM)next, TRUE);
        for (auto &strip : strips)
            if (strip)
                strip->RefreshDragFont(next);
        if (font)
            DeleteObject(font);
        font = next;
    }
    auto stock = [&](HICON &owned, SHSTOCKICONID id) {
        SHSTOCKICONINFO info{sizeof(info)};
        if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_SYSICONINDEX, &info)))
            if (HICON icon = SystemImageIcon(info.iSysImageIndex, S(16))) {
                if (owned)
                    DestroyIcon(owned);
                owned = icon;
            }
    };
    stock(folderIcon, SIID_FOLDER);
    stock(computerIcon, SIID_DESKTOPPC);
    stock(missingIcon, SIID_ERROR);
    // 只用已有索引刷新图像，不因 DPI 变化重新访问网络目录。
    for (auto &[id, owned] : icons) {
        auto it = iconIndices.find(id);
        HICON icon = it != iconIndices.end() ? SystemImageIcon(it->second, S(16)) : nullptr;
        if (!icon)
            icon = CopyIcon(model.Find(id) && model.Find(id)->path == Home ? computerIcon : folderIcon);
        if (icon) {
            if (owned)
                DestroyIcon(owned);
            owned = icon;
        }
    }
    Log(L"更新 DPI 资源 dpi=" + std::to_wstring(dpi));
}
void App::Layout(bool updateStatus) {
    if (!status)
        return;
    RECT rc;
    GetClientRect(window, &rc);
    SendMessageW(status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(status, &sr);
    int bottom = rc.bottom - (sr.bottom - sr.top), top = S(38);
    for (int i = 0; i < 4; ++i)
        MoveWindow(GetDlgItem(window, Back + i), S(6 + i * 36), S(3), S(32), S(32), TRUE);
    int middle = (int)(rc.right * model.split);
    for (int p = 0; p < 2; ++p) {
        int left = p ? middle + S(3) : 0, width = p ? rc.right - left : middle - S(3);
        width = std::max(width, 1);
        int tabHeight = strips[p] ? strips[p]->Arrange(width) : S(32);
        if (strips[p]) {
            MoveWindow(strips[p]->hwnd, left, top, width, tabHeight, TRUE);
            InvalidateRect(strips[p]->hwnd, nullptr, FALSE);
        }
        int y = top + tabHeight;
        MoveWindow(crumb[p], left, y, width, S(32), TRUE);
        MoveWindow(address[p], left + S(34), y + S(6), std::max(1, width - S(40)), S(20), TRUE);
        InvalidateRect(crumb[p], nullptr, TRUE);
        RECT view{left, y + S(34), left + width, std::max(y + S(35), bottom)};
        viewBounds[p] = view;
        MoveWindow(error[p], left + S(12), view.top + S(15), std::max(width - S(24), 1), S(90), TRUE);
        bool failed = false;
        for (auto &[id, v] : views)
            if (model.PaneOf(id) == p) {
                bool selected = id == model.panes[p].selected;
                v->Show(selected && SUCCEEDED(v->error));
                if (selected) {
                    v->Resize(view);
                    failed = FAILED(v->error);
                }
            }
        ShowWindow(error[p], failed ? SW_SHOW : SW_HIDE);
        UpdateAddress(p);
    }
    if (updateStatus)
        UpdateStatus();
}
void App::EnsureView(Id id) {
    if (!model.Find(id) || views.contains(id))
        return;
    Layout(false);
    RefreshLocalIcon(id);
    auto v = std::make_unique<ShellView>(*this, id);
    auto *ptr = v.get();
    views[id] = std::move(v);
    HRESULT hr = ptr->Create(window, viewBounds[model.PaneOf(id)]);
    if (FAILED(hr)) {
        ShowError(id, hr);
        return;
    }
    Layout(false);
    ptr->Browse(model.Find(id)->path, false);
}
void App::Activate(int p, Id id, bool focus) {
    if (p < 0 || p > 1)
        return;
    if (model.active != p || (id && model.panes[p].selected != id))
        ++pasteGeneration;
    model.active = p;
    if (id && model.PaneOf(id) == p)
        model.panes[p].selected = id;
    model.EnsureVisibleSelection(p);
    EnsureView(model.panes[p].selected);
    Layout();
    Changed();
    if (focus) {
        auto it = views.find(model.panes[p].selected);
        if (it != views.end() && it->second->shell) {
            HWND w;
            it->second->shell->GetWindow(&w);
            SetFocus(w);
        }
    }
}
void App::Changed() {
    if (!readOnly)
        dirty = true;
}
void App::Save() {
    if (readOnly || !window)
        return;
    for (auto &[id, v] : views)
        v->Capture();
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (GetWindowPlacement(window, &wp)) {
        model.window = wp.rcNormalPosition;
        model.windowDpi = dpi;
        model.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    }
    if (SaveModel(model, ConfigDir() / L"session.dat")) {
        dirty = false;
        saveFailures = 0;
    } else {
        Log(L"会话保存失败");
        if (!saveFailures++)
            MessageBoxW(window, L"无法保存会话，请检查配置目录权限或磁盘空间。", L"ZenExplorer",
                        MB_OK | MB_ICONWARNING);
    }
}
void App::UpdateAddress(int p) {
    auto *t = model.Find(model.panes[p].selected);
    if (!t)
        return;
    if (GetFocus() != address[p])
        SetWindowTextW(address[p], t->path.c_str());
    InvalidateRect(crumb[p], nullptr, FALSE);
}
void App::EditAddress(int p) {
    if (p < 0 || p > 1)
        return;
    if (GetWindowLongPtrW(address[p], GWL_STYLE) & WS_VISIBLE) {
        SetFocus(address[p]);
        return;
    }
    Activate(p);
    UpdateAddress(p);
    ShowWindow(address[p], SW_SHOW);
    SetWindowPos(address[p], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(crumb[p], nullptr, FALSE);
    SetFocus(address[p]);
    SendMessageW(address[p], EM_SETSEL, 0, -1);
}
void App::Navigate(Id id, const std::wstring &path, bool append) {
    ++pasteGeneration;
    auto *t = model.Find(id);
    if (!t || path.empty())
        return;
    EnsureView(id);
    auto it = views.find(id);
    if (it != views.end()) {
        // 先保存旧视图，再应用地址改变时的模式重置，避免旧模式覆盖新状态。
        it->second->Capture();
        model.Navigate(id, path, append);
        RefreshLocalIcon(id);
        Changed();
        it->second->Browse(path, false);
        Layout();
    }
}
void App::Travel(int delta) {
    auto *t = model.Find(model.panes[model.active].selected);
    if (!t)
        return;
    int next = t->historyIndex + delta;
    if (next >= 0 && next < (int)t->history.size()) {
        t->historyIndex = next;
        auto path = t->history[next];
        Navigate(t->id, path, false);
    }
}
void App::NewTab(int p, const std::wstring &path, Id group, int index, Id opener) {
    Id id = model.Add(p, path, group, index, true, opener);
    if (!id)
        return;
    if (group)
        model.SetGroupCollapsed(p, group, false);
    Activate(p, id);
    strips[p]->EnsureVisible(id);
    BuildMenu();
}
void App::SyncViews() {
    std::erase_if(views, [&](auto &pair) { return !model.Find(pair.first); });
    for (int p = 0; p < 2; ++p)
        EnsureView(model.panes[p].selected);
    Layout();
}
void App::CloseTabs(int p, const std::vector<Id> &ids, bool group) {
    bool named = false;
    for (auto id : ids)
        if (auto *t = model.Find(id))
            named |= !t->name.empty();
    if (named &&
        !Confirm(window, group ? L"当前关闭的组包含常用标签，是否确认" : L"当前关闭的是常用标签，是否确认"))
        return;
    model.Remove(p, ids, true, group);
    SyncViews();
    Changed();
    BuildMenu();
}
std::wstring App::Title(const Tab &t) const {
    return t.Title();
}
HICON App::Icon(Id id) const {
    auto it = icons.find(id);
    if (it != icons.end() && it->second)
        return it->second;
    auto *t = model.Find(id);
    return t && t->path == Home ? computerIcon : folderIcon;
}
void App::RefreshLocalIcon(Id id) {
    auto *t = model.Find(id);
    if (!t)
        return;
    HICON icon = nullptr;
    iconIndices.erase(id);
    auto state = InspectLocalPath(t->path);
    if (t->path == Home || state == LocalPathState::Missing) {
        SHSTOCKICONINFO info{sizeof(info)};
        if (SUCCEEDED(
                SHGetStockIconInfo(t->path == Home ? SIID_DESKTOPPC : SIID_ERROR, SHGSI_SYSICONINDEX, &info)))
            iconIndices[id] = info.iSysImageIndex;
    }
    if (t->path == Home)
        icon = CopyIcon(computerIcon);
    else if (state == LocalPathState::Missing)
        icon = CopyIcon(missingIcon);
    else if (state == LocalPathState::Exists) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(t->path.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME)) {
            iconIndices[id] = info.iIcon;
            icon = SystemImageIcon(info.iIcon, S(16));
            // 版本 1 没有显示名称缓存；复用已有本地图标查询补齐，不额外加载视图。
            if (t->displayName.empty() && info.szDisplayName[0]) {
                t->displayName = info.szDisplayName;
                Changed();
            }
        }
    }
    if (!icon)
        icon = CopyIcon(folderIcon);
    if (auto it = icons.find(id); it != icons.end() && it->second)
        DestroyIcon(it->second);
    icons[id] = icon;
}
void App::RefreshMetadata(Id id, IShellItem *item) {
    auto title = ItemName(item, SIGDN_NORMALDISPLAY);
    if (auto *tab = model.Find(id); tab && !title.empty() && tab->displayName != title) {
        tab->displayName = std::move(title);
        Changed();
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHGetIDListFromObject(item, &pidl))) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW((LPCWSTR)pidl, 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_SYSICONINDEX)) {
            iconIndices[id] = info.iIcon;
            if (HICON icon = SystemImageIcon(info.iIcon, S(16))) {
                if (icons[id])
                    DestroyIcon(icons[id]);
                icons[id] = icon;
            }
        }
        CoTaskMemFree(pidl);
    }
}
void App::ShowError(Id id, HRESULT hr) {
    RefreshLocalIcon(id);
    auto it = views.find(id);
    if (it != views.end())
        it->second->error = hr;
    int p = model.PaneOf(id);
    if (p >= 0 && model.panes[p].selected == id) {
        auto text = L"目录无法访问\n" + ErrorText(hr) + L"\n点击重试";
        SetWindowTextW(error[p], text.c_str());
    }
    Log(L"目录访问失败 " + ErrorText(hr));
    Layout();
}
void App::UpdateStatus() {
    if (!status)
        return;
    auto *t = model.Find(model.panes[model.active].selected);
    if (!t)
        return;
    EnableWindow(GetDlgItem(window, Back), t->historyIndex > 0);
    EnableWindow(GetDlgItem(window, Forward), t->historyIndex + 1 < (int)t->history.size());
    auto it = views.find(t->id);
    EnableWindow(GetDlgItem(window, Up), it != views.end() && it->second->CanUp());
    int total = 0, selected = 0;
    uint64_t bytes = 0;
    bool unknown = false;
    if (it != views.end()) {
        auto &v = *it->second;
        ComPtr<IFolderView2> f;
        if (v.shell && SUCCEEDED(v.shell.As(&f))) {
            f->ItemCount(SVGIO_ALLVIEW, &total);
            auto items = v.Selected();
            selected = (int)items.size();
            for (auto &item : items) {
                SFGAOF attr = 0;
                item->GetAttributes(SFGAO_FOLDER, &attr);
                if (attr & SFGAO_FOLDER)
                    continue;
                ComPtr<IShellItem2> s;
                if (SUCCEEDED(item.As(&s))) {
                    ULONGLONG size;
                    if (SUCCEEDED(s->GetUInt64(PKEY_Size, &size)))
                        bytes += size;
                    else
                        unknown = true;
                }
            }
        }
    }
    auto text = std::to_wstring(total) + L" 个项目　已选择 " + std::to_wstring(selected) + L" 个项目　" +
                SizeText(bytes) + (unknown ? L"（部分大小未知）" : L"");
    SendMessageW(status, SB_SETTEXTW, 0, (LPARAM)text.c_str());
}
void App::BuildMenu() {
    HMENU main = CreateMenu(), file = CreatePopupMenu(), recent = CreatePopupMenu(), view = CreatePopupMenu(),
          help = CreatePopupMenu();
    for (size_t i = 0; i < model.recent.size(); ++i)
        AppendMenuW(recent, MF_STRING, RecentBase + i, model.recent[i].c_str());
    if (model.recent.empty())
        AppendMenuW(recent, MF_STRING | MF_GRAYED, 0, L"暂无记录");
    AppendMenuW(recent, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(recent, MF_STRING, ClearRecent, L"清空记录");
    AppendMenuW(file, MF_POPUP, (UINT_PTR)recent, L"最近访问目录");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, Exit, L"退出");
    const wchar_t *modes[] = {L"详细信息", L"列表", L"小图标", L"中等图标", L"大图标", L"超大图标"};
    for (int i = 0; i < 6; ++i)
        AppendMenuW(view, MF_STRING, ViewBase + i, modes[i]);
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING | (model.wrap ? MF_CHECKED : 0), Wrap, L"标签自动换行");
    AppendMenuW(view, MF_STRING | (model.singleExpandedGroup ? MF_CHECKED : 0), SingleGroup,
                L"每栏仅展开一个组");
    AppendMenuW(help, MF_STRING, About, L"关于");
    AppendMenuW(main, MF_POPUP, (UINT_PTR)file, L"文件");
    AppendMenuW(main, MF_POPUP, (UINT_PTR)view, L"查看");
    AppendMenuW(main, MF_POPUP, (UINT_PTR)help, L"帮助");
    auto old = GetMenu(window);
    SetMenu(window, main);
    if (old)
        DestroyMenu(old);
    DrawMenuBar(window);
}
void App::Command(UINT id) {
    if (id == Back)
        Travel(-1);
    else if (id == Forward)
        Travel(1);
    else if (id == Up || id == Refresh) {
        auto it = views.find(model.panes[model.active].selected);
        if (it != views.end()) {
            if (id == Up)
                it->second->Up();
            else
                it->second->Refresh();
        }
    } else if (id == Exit)
        SendMessageW(window, WM_CLOSE, 0, 0);
    else if (id == Wrap) {
        model.wrap = !model.wrap;
        for (auto &s : strips)
            s->scroll = 0;
        Changed();
        BuildMenu();
        Layout();
    } else if (id == SingleGroup) {
        model.SetSingleExpandedGroup(!model.singleExpandedGroup);
        Changed();
        BuildMenu();
        SyncViews();
    } else if (id == ClearRecent) {
        if (Confirm(window, L"是否清空最近访问目录？")) {
            model.recent.clear();
            Changed();
            BuildMenu();
        }
    } else if (id >= RecentBase && id < RecentBase + model.recent.size()) {
        auto path = model.recent[id - RecentBase];
        NewTab(model.active, path);
    } else if (id >= ViewBase && id < ViewBase + 6) {
        auto *t = model.Find(model.panes[model.active].selected);
        if (t) {
            int n = id - ViewBase;
            auto previousMode = t->viewMode;
            auto previousSize = t->iconSize;
            t->viewMode = n == 0 ? FVM_DETAILS : n == 1 ? FVM_LIST : FVM_ICON;
            int sizes[] = {16, 16, 16, 48, 96, 256};
            t->iconSize = sizes[n];
            auto it = views.find(t->id);
            if (it != views.end()) {
                HRESULT hr = it->second->ApplyViewMode();
                if (FAILED(hr)) {
                    t->viewMode = previousMode;
                    t->iconSize = previousSize;
                    Report(window, L"无法切换系统查看模式", hr);
                    return;
                }
            }
            Changed();
        }
    } else if (id == About)
        MessageBoxW(window, L"ZenExplorer 0.1\n原生双栏文件管理器\n文件视图和文件操作由 Windows Shell 提供。",
                    L"关于", MB_OK);
}

void App::TabMenu(int p, Id tab, Id group, POINT pt) {
    HMENU menu = CreatePopupMenu();
    std::vector<HBITMAP> colorBitmaps;
    constexpr UINT ColorBase = 20;
    if (tab) {
        auto *t = model.Find(tab);
        AppendMenuW(menu, MF_STRING, 1, t && !t->name.empty() ? L"修改常用标签名称…" : L"设为常用标签…");
        if (t && !t->name.empty()) {
            AppendMenuW(menu, MF_STRING, 2, L"将当前目录设为主目录");
            AppendMenuW(menu, MF_STRING, 6, L"取消常用标签");
        }
        AppendMenuW(menu, MF_STRING, 5, L"在系统资源管理器中打开");
        AppendMenuW(menu, MF_STRING, 3, L"添加到新组");
        HMENU groups = CreatePopupMenu();
        for (size_t i = 0; i < model.panes[p].groups.size(); ++i)
            AppendMenuW(groups, MF_STRING, 100 + i, model.panes[p].groups[i].name.c_str());
        if (!model.panes[p].groups.empty())
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)groups, L"添加到现有组");
        else
            DestroyMenu(groups);
        if (t && t->group)
            AppendMenuW(menu, MF_STRING, 4, L"移出组");
    } else if (group) {
        AppendMenuW(menu, MF_STRING, 10, L"修改组名");
        HMENU colors = CreatePopupMenu();
        auto *current = model.FindGroup(p, group);
        for (UINT i = 0; i < std::size(GroupColors); ++i) {
            const auto &preset = GroupColors[i];
            AppendMenuW(colors, MF_STRING | (current && current->color == preset.color ? MF_CHECKED : 0),
                        ColorBase + i, preset.name);
            // 位图在菜单关闭后统一释放，色块与系统勾选标记各占自己的位置。
            HDC screen = GetDC(window);
            HDC dc = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, S(14), S(14));
            if (dc && bitmap) {
                auto old = SelectObject(dc, bitmap);
                RECT rect{0, 0, S(14), S(14)};
                auto brush = CreateSolidBrush(preset.color);
                FillRect(dc, &rect, brush);
                DeleteObject(brush);
                SelectObject(dc, old);
                MENUITEMINFOW item{sizeof(item)};
                item.fMask = MIIM_BITMAP;
                item.hbmpItem = bitmap;
                SetMenuItemInfoW(colors, ColorBase + i, FALSE, &item);
                colorBitmaps.push_back(bitmap);
            } else if (bitmap)
                DeleteObject(bitmap);
            if (dc)
                DeleteDC(dc);
            ReleaseDC(window, screen);
        }
        AppendMenuW(colors, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(colors, MF_STRING, 11, L"自定义颜色…");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)colors, L"修改颜色");
        AppendMenuW(menu, MF_STRING, 12, L"取消分组");
        AppendMenuW(menu, MF_STRING, 13, L"在组内新建标签");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 14, L"关闭整组");
    }
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
    DestroyMenu(menu);
    for (auto bitmap : colorBitmaps)
        DeleteObject(bitmap);
    if (!cmd)
        return;
    if (tab) {
        auto *t = model.Find(tab);
        if (!t)
            return;
        if (cmd == 5) {
            OpenInExplorer(t->path);
            return;
        }
        if (cmd == 1) {
            auto value = Input(
                window, t->name.empty() ? L"设为常用标签" : L"修改常用标签名称", Title(*t),
                t->name.empty()
                    ? L"设置标签名称，并以当前目录为主目录。浏览其他目录时名称不变，双击标签返回主目录。"
                    : L"修改名称不会改变主目录；清空名称或使用右键菜单可取消常用标签。");
            if (value)
                model.Rename(tab, *value);
        } else if (cmd == 2) {
            t->home = t->path;
        } else if (cmd == 6) {
            model.Rename(tab, L"");
        } else if (cmd == 3) {
            auto value = Input(window, L"组名", L"新建组");
            // 取消输入不创建组，也不改变标签原来的分组和位置。
            if (!value)
                return;
            Id g = model.MakeGroup(p, tab);
            if (auto *created = model.FindGroup(p, g); created && !value->empty())
                created->name = *value;
        } else if (cmd == 4)
            model.AssignGroup(tab, 0);
        else if (cmd >= 100 && cmd < 100 + model.panes[p].groups.size()) {
            Id target = model.panes[p].groups[cmd - 100].id;
            model.AssignGroup(tab, target);
        }
    } else if (auto *g = model.FindGroup(p, group)) {
        if (cmd == 10) {
            auto name = Input(window, L"组名", g->name);
            if (name && !name->empty())
                g->name = *name;
        } else if (cmd >= ColorBase && cmd < ColorBase + std::size(GroupColors)) {
            g->color = GroupColors[cmd - ColorBase].color;
            Log(L"设置组预设颜色 group=" + std::to_wstring(group));
        } else if (cmd == 11) {
            static COLORREF custom[16]{};
            CHOOSECOLORW c{sizeof(c)};
            c.hwndOwner = window;
            c.rgbResult = g->color;
            c.lpCustColors = custom;
            c.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&c)) {
                g->color = c.rgbResult;
                Log(L"设置组自定义颜色 group=" + std::to_wstring(group));
            }
        } else if (cmd == 12)
            model.Ungroup(p, group);
        else if (cmd == 13) {
            int index = 0;
            for (int i = 0; i < (int)model.panes[p].tabs.size(); ++i)
                if (model.panes[p].tabs[i].group == group)
                    index = i + 1;
            NewTab(p, Home, group, index);
        } else if (cmd == 14) {
            std::vector<Id> ids;
            for (auto &t : model.panes[p].tabs)
                if (t.group == group)
                    ids.push_back(t.id);
            CloseTabs(p, ids, true);
        }
    }
    Changed();
    SyncViews();
}
void App::OpenInExplorer(const std::wstring &path) {
    auto item = ParseItem(path);
    SFGAOF attributes = 0;
    HRESULT hr =
        item ? item->GetAttributes(SFGAO_FOLDER, &attributes) : HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    if (SUCCEEDED(hr) && !(attributes & SFGAO_FOLDER))
        hr = HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    if (SUCCEEDED(hr)) {
        wchar_t windows[MAX_PATH]{};
        UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
        if (!length || length >= MAX_PATH)
            hr = E_FAIL;
        else {
            // 明确启动系统 Explorer，不依赖可能被第三方替换的文件夹默认打开关联。
            auto executable = (std::filesystem::path(windows) / L"explorer.exe").wstring();
            auto target = ItemName(item.Get());
            if (target.empty())
                hr = E_INVALIDARG;
            else {
                auto parameters = Quote(target);
                SHELLEXECUTEINFOW info{sizeof(info)};
                info.fMask = SEE_MASK_FLAG_NO_UI;
                info.hwnd = window;
                info.lpFile = executable.c_str();
                info.lpParameters = parameters.c_str();
                info.nShow = SW_SHOWNORMAL;
                if (!ShellExecuteExW(&info))
                    hr = HRESULT_FROM_WIN32(GetLastError());
            }
        }
    }
    Log(L"系统资源管理器打开 path=" + path + L" result=" + std::to_wstring((unsigned long)hr));
    if (FAILED(hr))
        Report(window, L"无法在系统资源管理器中打开：" + path, hr);
}
void App::OpenSelected(ShellView &v, bool newTab) {
    auto items = v.Selected();
    std::vector<std::wstring> paths;
    for (auto &i : items) {
        SFGAOF attr = 0;
        i->GetAttributes(SFGAO_FOLDER, &attr);
        if (!(attr & SFGAO_FOLDER))
            return;
        paths.push_back(ItemName(i.Get()));
    }
    if (paths.empty())
        return;
    if (!newTab) {
        Navigate(v.id, paths.front());
        return;
    }
    int p = model.PaneOf(v.id), index = model.Index(p, v.id) + 1;
    Id group = model.Find(v.id)->group, first = 0;
    for (auto &s : paths) {
        Id id = model.Add(p, s, group, index++, false, v.id);
        if (!first)
            first = id;
    }
    if (group)
        model.SetGroupCollapsed(p, group, false);
    Activate(p, first);
    strips[p]->EnsureVisible(first);
}
HRESULT App::ShellVerb(ShellView &v, const std::wstring &verb, bool background,
                       std::optional<DWORD> modifiers) {
    ShellMenu menu;
    HRESULT hr = menu.Create(v.shell.Get(), background, window);
    if (FAILED(hr))
        return hr;
    UINT command = menu.Find(verb);
    if (!command)
        return S_FALSE;
    return menu.Invoke(command, {}, modifiers);
}
void App::CopySelection(ShellView &v, bool cut) {
    HRESULT hr = ShellVerb(v, cut ? L"cut" : L"copy", false);
    if (hr == S_OK)
        hr = OleFlushClipboard();
    if (FAILED(hr))
        Report(window, cut ? L"系统剪切命令失败" : L"系统复制命令失败", hr);
}
bool App::PasteFeedbackCurrent(const PendingPaste &pending, HWND foreground, HWND focus) {
    auto it = views.find(pending.tab);
    return pending.pane >= 0 && pending.pane < 2 && pasteGeneration == pending.generation &&
           model.active == pending.pane && model.PaneOf(pending.tab) == pending.pane &&
           model.panes[pending.pane].selected == pending.tab && it != views.end() &&
           !it->second->navigating && ItemName(it->second->Current().Get()) == pending.destination &&
           focus == pending.focus && foreground == window && GetTickCount64() <= pending.deadline;
}
void App::PollPastes() {
    std::erase_if(pendingDrops, [&](PendingDrop &pending) {
        std::error_code ec;
        bool ready = std::filesystem::exists(pending.result, ec);
        if (!ready && (!pending.process || WaitForSingleObject(pending.process.get(), 0) != WAIT_OBJECT_0))
            return false;
        if (!ready)
            ready = std::filesystem::exists(pending.result, ec);
        bool cancelled = false;
        if (ready) {
            try {
                auto result = ReadTaskResult(pending.result);
                cancelled = TaskCancelled(result.invoked) ||
                            (SUCCEEDED(result.invoked) && result.effect == DROPEFFECT_NONE);
            } catch (...) {
                Log(L"拖放结果不可用，不猜测完成状态");
            }
        } else
            Log(L"拖放操作进程退出但没有结果");
        // 用户已经继续交互时，不用迟到结果覆盖其分组选择。
        if (cancelled && pending.generation == dropGeneration) {
            for (const auto &state : pending.expanded)
                model.RestoreGroups(state);
            Changed();
            SyncViews();
        }
        return true;
    });
    std::erase_if(pendingPastes, [&](PendingPaste &pending) {
        std::error_code ec;
        if (!std::filesystem::exists(pending.result, ec)) {
            if (pending.process && WaitForSingleObject(pending.process.get(), 0) == WAIT_OBJECT_0) {
                // 进程退出可能早于本次 exists；重新检查，避免丢弃刚写完的结果。
                if (std::filesystem::exists(pending.result, ec))
                    return false;
                Log(L"操作进程已退出但没有结果，保留剪贴板和选择 " + pending.result.wstring());
                return true;
            }
            return false;
        }
        try {
            if (!pending.read) {
                auto result = ReadTaskResult(pending.result);
                if (FAILED(result.invoked))
                    return true;
                // 复制也跟踪反馈，但仅剪切完成后清理仍对应本次操作的剪贴板。
                if (pending.move && result.moveCompleted &&
                    GetClipboardSequenceNumber() == pending.clipboardSequence)
                    OleSetClipboard(nullptr);
                if (!pending.tab)
                    return true;
                auto detailPath = pending.result;
                detailPath += L".items";
                // Shell 命令未提供实际结果明细时，不猜测冲突改名后的文件名称。
                if (!std::filesystem::exists(detailPath, ec))
                    return true;
                auto details = ReadTaskItems(pending.result);
                if (details.destination != pending.destination || details.created.empty())
                    return true;
                pending.created = std::move(details.created);
                pending.read = true;
                pending.deadline = GetTickCount64() + 5000;
            }
            if (!PasteFeedbackCurrent(pending, GetForegroundWindow(), GetFocus()))
                return true;
            auto it = views.find(pending.tab);
            HRESULT selection = it->second->SelectPasteResults(pending.created);
            if (selection == S_FALSE)
                return false;
            Log(L"粘贴结果选中 tab=" + std::to_wstring(pending.tab) + L" result=" +
                std::to_wstring((unsigned long)selection));
        } catch (...) {
            Log(L"粘贴结果或可选明细不可用，跳过自动选中");
        }
        return true;
    });
}
void App::Paste(ShellView &v, IShellItem *destination) {
    ++pasteGeneration;
    ComPtr<IDataObject> data;
    HRESULT clipboardHr = OleGetClipboard(&data);
    if (FAILED(clipboardHr)) {
        Log(L"粘贴无法取得剪贴板 " + ErrorText(clipboardHr));
        return;
    }
    auto current = destination ? ComPtr<IShellItem>(destination) : v.Current();
    std::vector<std::wstring> paths;
    SFGAOF flags = 0;
    HRESULT targetHr = current ? current->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &flags) : E_POINTER;
    bool fileTarget = SUCCEEDED(targetHr) &&
                      (flags & (SFGAO_FILESYSTEM | SFGAO_FOLDER)) == (SFGAO_FILESYSTEM | SFGAO_FOLDER);
    if (!fileTarget)
        Log(L"粘贴回退原因：目标不是可用文件系统目录 flags=" + std::to_wstring(flags) + L" " +
            ErrorText(targetHr));
    bool independent = fileTarget && SnapshotFiles(data.Get(), paths);
    if (!independent) {
        Report(window, L"此来源或目标尚不能独立执行，操作未开始。", E_NOTIMPL);
        return;
    }
    auto dest = ItemName(current.Get());
    if (dest.empty())
        return;
    bool move = (PreferredEffect(data.Get()) & DROPEFFECT_MOVE) != 0;
    std::filesystem::path result;
    DWORD sequence = GetClipboardSequenceNumber();
    PendingPaste pending{{}, sequence};
    pending.move = move;
    pending.pane = model.PaneOf(v.id);
    pending.generation = pasteGeneration;
    pending.focus = GetFocus();
    pending.destination = dest;
    wchar_t focusClass[80]{};
    GetClassNameW(pending.focus, focusClass, 80);
    if (pending.pane == model.active && model.panes[pending.pane].selected == v.id &&
        ItemName(v.Current().Get()) == dest && _wcsicmp(focusClass, L"Edit") &&
        GetForegroundWindow() == window)
        pending.tab = v.id;
    Log(L"粘贴反馈准备 tab=" + std::to_wstring(v.id) + L" enabled=" + std::to_wstring(pending.tab != 0) +
        L" foreground=" + std::to_wstring(GetForegroundWindow() == window) + L" focus=" + focusClass +
        L" pane=" + std::to_wstring(pending.pane) + L" active=" + std::to_wstring(model.active));
    FileTask task{Operation::Paste, dest, paths, false};
    task.preferred = PreferredEffect(data.Get());
    task.collectResults = dest == ItemName(v.Current().Get());
    if (LaunchTask(window, task, {}, &result, &pending.process)) {
        pending.result = result;
        pendingPastes.push_back(std::move(pending));
    }
}
void App::DeleteSelection(ShellView &v, bool permanent) {
    auto data = v.SelectedData();
    std::vector<std::wstring> paths;
    if (SnapshotFiles(data.Get(), paths))
        LaunchTask(window, {Operation::Delete, L"", paths, permanent});
    else
        Report(window, L"此项目尚不能独立删除，操作未开始。", E_NOTIMPL);
}

void App::FileDrop(ShellView &v, IDataObject *data, DWORD key, POINTL point, DWORD *effect) {
    if (!data || !effect)
        return;
    bool delivered = false;
    struct FinishDrop {
        App &app;
        bool &delivered;
        ~FinishDrop() {
            if (!delivered)
                for (auto &strip : app.strips)
                    strip->FinishExternal(false);
        }
    } finish{*this, delivered};
    DWORD allowed = *effect;
    *effect = DROPEFFECT_NONE;
    if (key & MK_RBUTTON)
        return;
    auto destination = v.DropItem(point);
    SFGAOF attributes = 0;
    if (!destination || FAILED(destination->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes)) ||
        (attributes & (SFGAO_FILESYSTEM | SFGAO_FOLDER)) != (SFGAO_FILESYSTEM | SFGAO_FOLDER))
        return;
    ComPtr<IDropTarget> target;
    HRESULT hr = SystemDropTarget(destination.Get(), &target);
    if (FAILED(hr))
        return;
    DWORD chosen = allowed;
    // 使用系统目标协商最终效果；不执行它的 Drop，不自行按盘符决定复制/移动。
    hr = target->DragEnter(data, key, point, &chosen);
    target->DragLeave();
    if (FAILED(hr) || !(allowed & chosen) || (chosen != DROPEFFECT_COPY && chosen != DROPEFFECT_MOVE))
        return;
    std::vector<std::wstring> paths;
    if (!SnapshotFiles(data, paths)) {
        Log(L"拖放来源尚不能独立重建，拒绝在主窗口执行");
        return;
    }
    FileTask task{Operation::Drop, ItemName(destination.Get()), paths, false};
    task.keys = key;
    task.point = point;
    task.allowed = allowed;
    task.preferred = chosen;
    task.collectResults = false;
    PendingDrop pending;
    if (!LaunchTask(window, task, {}, &pending.result, &pending.process, data, effect))
        return;
    delivered = true;
    Log(L"拖放独立交付 effect=" + std::to_wstring(chosen) + L" destination=" + task.destination);
    for (auto &strip : strips) {
        auto expanded = strip->DetachExternal();
        pending.expanded.insert(pending.expanded.end(), expanded.begin(), expanded.end());
    }
    pending.generation = dropGeneration;
    pendingDrops.push_back(std::move(pending));
    Changed();
}
void App::FileMenu(ShellView &v, POINT pt, bool background) {
    int pane = model.PaneOf(v.id);
    if (pane < 0 || !v.shell)
        return;
    model.active = pane;
    // 背景菜单与残留选择无关，不能把当前目录命令误投递给选中的子文件夹。
    auto selected = background ? std::vector<ComPtr<IShellItem>>{} : v.Selected();
    if (!background && selected.empty())
        return;
    bool folders = !selected.empty();
    for (auto &i : selected) {
        SFGAOF attr = 0;
        if (FAILED(i->GetAttributes(SFGAO_FOLDER, &attr)) || !(attr & SFGAO_FOLDER))
            folders = false;
    }
    ShellMenu session;
    HRESULT hr = session.Create(v.shell.Get(), background, window,
                                CMF_NORMAL | ((GetKeyState(VK_SHIFT) & 0x8000) ? CMF_EXTENDEDVERBS : 0));
    if (FAILED(hr)) {
        Report(window, L"无法创建系统菜单", hr);
        return;
    }
    HMENU menu = session.menu;
    auto verbOf = [&](UINT id) { return session.Verb(id); };
    std::function<void(HMENU)> prune = [&](HMENU m) {
        for (int i = GetMenuItemCount(m) - 1; i >= 0; --i) {
            UINT id = GetMenuItemID(m, i);
            auto verb = verbOf(id);
            if ((folders || selected.empty()) && (verb == L"opennewwindow" || verb == L"opennewprocess" ||
                                                  verb == L"opennewtab" || verb == L"explore"))
                DeleteMenu(m, i, MF_BYPOSITION);
            else if (auto sub = GetSubMenu(m, i))
                prune(sub);
        }
    };
    prune(menu);
    if (folders) {
        InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING, 0x7001, L"在新标签中打开");
    }
    if (folders || selected.empty())
        InsertMenuW(menu, folders ? 1 : 0, MF_BYPOSITION | MF_STRING, 0x7002, L"在系统资源管理器中打开");
    // 支持嵌套菜单，并在命令执行结束后恢复外层的消息转发目标。
    auto previous2 = menu2;
    auto previous3 = menu3;
    struct RestoreMenu {
        App &app;
        ComPtr<IContextMenu2> old2;
        ComPtr<IContextMenu3> old3;
        ~RestoreMenu() {
            app.menu2 = old2;
            app.menu3 = old3;
        }
    } restore{*this, previous2, previous3};
    menu2 = session.menu2;
    menu3 = session.menu3;
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
    if (!cmd)
        return;
    if (cmd == 0x7001) {
        OpenSelected(v, true);
        return;
    }
    if (cmd == 0x7002) {
        if (selected.empty())
            OpenInExplorer(ItemName(v.Current().Get()));
        else if (folders)
            for (auto &item : selected)
                OpenInExplorer(ItemName(item.Get()));
        return;
    }
    auto verb = verbOf(cmd);
    if (folders && verb == L"open") {
        OpenSelected(v, false);
        return;
    }
    if (verb == L"copy" || verb == L"cut") {
        CopySelection(v, verb == L"cut");
        return;
    }
    if (verb == L"paste") {
        // 项目菜单的目标是右键选中的文件夹；快捷键与背景菜单仍使用当前目录。
        if (selected.empty())
            Paste(v);
        else if (folders && selected.size() == 1)
            Paste(v, selected.front().Get());
        else
            Report(window, L"请选择一个目标文件夹后粘贴。");
        return;
    }
    if (verb == L"delete") {
        DeleteSelection(v, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return;
    }
    hr = session.Invoke(cmd, pt);
    if (FAILED(hr))
        Report(window, L"系统菜单命令执行失败", hr);
}
void App::ViewMouse(const MSG &msg) {
    if (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN || msg.message == WM_MBUTTONDOWN ||
        msg.message == WM_MOUSEWHEEL || msg.message == WM_MOUSEHWHEEL) {
        ++pasteGeneration;
        ++dropGeneration;
    }
    if (msg.message != WM_LBUTTONDOWN && msg.message != WM_RBUTTONDOWN && msg.message != WM_MBUTTONDOWN)
        return;
    for (int p = 0; p < 2; ++p) {
        auto it = views.find(model.panes[p].selected);
        if (it == views.end())
            continue;
        auto &view = *it->second;
        if (msg.hwnd != view.host && !IsChild(view.host, msg.hwnd))
            continue;
        // 空目录提示不一定产生 WM_SETFOCUS；在分发鼠标消息前确定所属栏。
        if (model.active != p)
            Activate(p);
        HWND focus = GetFocus();
        if (view.shell && focus != view.host && !IsChild(view.host, focus)) {
            view.shell->UIActivate(SVUIA_ACTIVATE_FOCUS);
            focus = GetFocus();
            if (focus != view.host && !IsChild(view.host, focus)) {
                HWND viewWindow = nullptr;
                if (SUCCEEDED(view.shell->GetWindow(&viewWindow)))
                    SetFocus(viewWindow);
            }
        }
        Log(L"文件区点击 pane=" + std::to_wstring(p) + L" tab=" + std::to_wstring(view.id) + L" path=" +
            model.Find(view.id)->path);
        return;
    }
}
bool App::Key(MSG &msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN)
        return false;
    ++pasteGeneration;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    ++dropGeneration;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
    bool plainCtrl = ctrl && !alt && !shift && !win;
    HWND focus = GetFocus();
    // 文件快捷键以真实焦点所属的可见视图为准，防止旧活动栏状态误投递。
    for (int pane = 0; pane < 2; ++pane) {
        auto it = views.find(model.panes[pane].selected);
        if (it != views.end() && (focus == it->second->host || IsChild(it->second->host, focus))) {
            if (model.active != pane)
                Activate(pane);
            break;
        }
    }
    int p = model.active;
    if (ctrl && msg.wParam == 'L') {
        EditAddress(p);
        return true;
    }
    if (focus == address[0] || focus == address[1])
        return false;
    wchar_t cls[80]{};
    GetClassNameW(focus, cls, 80);
    if (!_wcsicmp(cls, L"Edit"))
        return false;
    // 文字编辑已在前面放行；独立于 Shell 的目录父子关系，复用本标签历史。
    if (msg.wParam == VK_BACK && !ctrl && !alt && !shift && !win) {
        Travel(-1);
        return true;
    }
    if (ctrl && (msg.wParam == 'W' || msg.wParam == 'T' || msg.wParam == VK_TAB))
        return true;
    if (alt && msg.wParam == VK_LEFT) {
        Travel(-1);
        return true;
    }
    if (alt && msg.wParam == VK_RIGHT) {
        Travel(1);
        return true;
    }
    if (alt && msg.wParam == VK_UP) {
        Command(Up);
        return true;
    }
    if (msg.wParam == VK_F5) {
        Command(Refresh);
        return true;
    }
    auto it = views.find(model.panes[p].selected);
    if (it == views.end())
        return false;
    auto &v = *it->second;
    bool inside = focus == v.host || IsChild(v.host, focus);
    if (inside) {
        if (plainCtrl && msg.wParam == 'C') {
            CopySelection(v, false);
            return true;
        }
        if (plainCtrl && msg.wParam == 'X') {
            CopySelection(v, true);
            return true;
        }
        if (plainCtrl && msg.wParam == 'Z') {
            HRESULT hr = ShellVerb(v, L"undo", true);
            if (FAILED(hr))
                Report(window, L"系统撤销失败", hr);
            return true;
        }
        if (plainCtrl && msg.wParam == 'V') {
            Paste(v);
            return true;
        }
        if (msg.wParam == VK_DELETE && !ctrl && !alt && !win) {
            DeleteSelection(v, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return true;
        }
        if (v.shell && v.shell->TranslateAccelerator(&msg) == S_OK)
            return true;
    }
    return false;
}
} // namespace zen
