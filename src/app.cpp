#include "app.h"
#include "operations.h"
#include "shell_view.h"
#include "shell_menu.h"
#include "tabs.h"
#include "ui_icons.h"
#include <commdlg.h>
#include <set>
#include <gdiplus.h>
#include <dwmapi.h>
namespace zen {
static constexpr UINT MenuBase = 120;
static constexpr UINT CaptionBase = 123;
static thread_local App *trackingMenu = nullptr;
static LRESULT CALLBACK MenuFilter(int code, WPARAM w, LPARAM l) {
    auto *app = trackingMenu;
    if (code == MSGF_MENU && app) {
        auto &msg = *reinterpret_cast<MSG *>(l);
        int next = -1;
        if (msg.message == WM_KEYDOWN && app->selectedMenu == GetSubMenu(app->mainMenu, app->menuIndex)) {
            if (msg.wParam == VK_LEFT)
                next = (app->menuIndex + 2) % 3;
            else if (msg.wParam == VK_RIGHT && !(app->selectedMenuFlags & MF_POPUP))
                next = (app->menuIndex + 1) % 3;
        }
        if (msg.message == WM_MOUSEMOVE) {
            POINT point = msg.pt;
            ScreenToClient(app->toolbar, &point);
            RECT bounds{};
            GetClientRect(app->toolbar, &bounds);
            if (PtInRect(&bounds, point)) {
                int hit = (int)SendMessageW(app->toolbar, TB_HITTEST, 0, (LPARAM)&point);
                if (hit >= 0 && hit < 3 && hit != app->menuIndex)
                    next = hit;
            }
        }
        if (next >= 0) {
            app->nextMenu = next;
            EndMenu();
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}
static constexpr UINT Back = 101, Forward = 102, Up = 103, Refresh = 104, Exit = 110, ClearRecent = 111,
                      Wrap = 112, About = 113, SingleGroup = 114, GroupSingleClick = 115,
                      GroupDoubleClick = 116, NavigationBase = 130, ViewBase = 200, RecentBase = 300;
static LRESULT CALLBACK ButtonHoverProc(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK)
        SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE)
        InvalidateRect(hwnd, nullptr, FALSE);
    return DefSubclassProc(hwnd, message, w, l);
}
App::App() : tasks(*this) {}
static void PaintCaptionButton(App &app, const DRAWITEMSTRUCT &item) {
    RECT r = item.rcItem;
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(item.hwndItem, &cursor);
    bool hot = PtInRect(&r, cursor), pressed = (item.itemState & ODS_SELECTED) != 0;
    bool close = item.CtlID == CaptionBase + 2;
    COLORREF bg = close && (hot || pressed) ? (pressed ? RGB(190, 20, 30) : RGB(232, 17, 35))
                  : pressed                 ? RGB(210, 210, 210)
                  : hot                     ? RGB(225, 225, 225)
                                            : GetSysColor(COLOR_BTNFACE);
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(item.hDC, &r, brush);
    HPEN pen = CreatePen(PS_SOLID, app.S(1),
                         close && (hot || pressed)            ? RGB(255, 255, 255)
                         : app.windowActive || hot || pressed ? GetSysColor(COLOR_WINDOWTEXT)
                                                              : RGB(128, 128, 128));
    auto oldPen = SelectObject(item.hDC, pen);
    auto oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    int x = (r.left + r.right) / 2 - app.S(5), y = (r.top + r.bottom) / 2 - app.S(5);
    auto line = [&](int x1, int y1, int x2, int y2) {
        MoveToEx(item.hDC, x + app.S(x1), y + app.S(y1), nullptr);
        LineTo(item.hDC, x + app.S(x2), y + app.S(y2));
    };
    if (item.CtlID == CaptionBase)
        line(0, 5, 10, 5);
    else if (close) {
        line(0, 0, 10, 10);
        line(10, 0, 0, 10);
    } else if (IsZoomed(app.window)) {
        Rectangle(item.hDC, x + app.S(2), y, x + app.S(10), y + app.S(8));
        RECT front{x, y + app.S(2), x + app.S(8), y + app.S(10)};
        FillRect(item.hDC, &front, brush);
        Rectangle(item.hDC, front.left, front.top, front.right, front.bottom);
    } else
        Rectangle(item.hDC, x, y, x + app.S(10), y + app.S(10));
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);
    if ((item.itemState & ODS_FOCUS) &&
        !(SendMessageW(item.hwndItem, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        InflateRect(&r, -app.S(4), -app.S(4));
        DrawFocusRect(item.hDC, &r);
    }
}
int App::TitleOffset() const {
    return customFrame && !IsZoomed(window)
               ? GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)
               : 0;
}
int App::TitleHeight() const {
    return TitleOffset() + S(customFrame ? 36 : 32);
}
void App::UpdateFrame() {
    if (!window)
        return;
    if (customFrame) {
        // 标题行使用普通不透明客户区，仅保留系统边框效果。
        MARGINS margins{0, 0, 1, 0};
        HRESULT hr = DwmExtendFrameIntoClientArea(window, &margins);
        if (FAILED(hr))
            Log(L"扩展标题区域失败 " + ErrorText(hr));
    }
    RECT title{};
    GetClientRect(window, &title);
    title.bottom = TitleHeight();
    InvalidateRect(window, &title, TRUE);
    if (toolbar)
        RedrawWindow(toolbar, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    for (int i = 0; i < 8; ++i)
        if (HWND button = GetDlgItem(window, NavigationBase + i))
            InvalidateRect(button, nullptr, TRUE);
    for (int i = 0; i < 3; ++i)
        if (HWND button = GetDlgItem(window, CaptionBase + i))
            InvalidateRect(button, nullptr, FALSE);
    for (HWND child : {AddressCrumb(0), AddressCrumb(1)})
        if (child)
            InvalidateRect(child, nullptr, FALSE);
}
App::~App() {
    addressBars[0].reset();
    addressBars[1].reset();
    if (mainMenu)
        DestroyMenu(mainMenu);
    views.clear();
    strips[0].reset();
    strips[1].reset();
    for (auto &[id, icon] : icons)
        if (icon)
            DestroyIcon(icon);
    for (auto &[id, icon] : homeIcons)
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
    model.quickAccess.Load(model.locations, ConfigDir() / L"quick-access.dat");
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
    customFrame = true;
    window =
        CreateWindowExW(0, cls.c_str(), L"ZenExplorer", (WS_OVERLAPPEDWINDOW & ~WS_CAPTION) | WS_CLIPCHILDREN,
                        r.left, r.top, width, height, nullptr, nullptr, h, this);
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
    UpdateFrame();
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RefreshWindowIcons();
    UpdateWindow(window);
    if (recovered) {
        Log(L"从有效备份恢复会话");
        MessageBoxW(window, L"会话文件异常，已从备份恢复。", L"ZenExplorer",
                    MB_OK | MB_ICONINFORMATION); // 避免下一次保存用损坏主文件覆盖有效备份。
    }
    if (future)
        MessageBoxW(window, L"部分配置无法安全读取。本次仅恢复可读取的数据，不会覆盖原配置。", L"ZenExplorer",
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
    if (menu3 && IsShellMenuMessage(m, a, b)) {
        LRESULT result = 0;
        if (SUCCEEDED(menu3->HandleMenuMsg2(m, a, b, &result)))
            return result;
    } else if (menu2 && m != WM_MENUCHAR && IsShellMenuMessage(m, a, b))
        if (SUCCEEDED(menu2->HandleMenuMsg(m, a, b)))
            return 0;
    switch (m) {
    case WM_NOTIFY: {
        auto *header = reinterpret_cast<NMHDR *>(b);
        if (header && header->hwndFrom == toolbar && header->code == NM_CUSTOMDRAW) {
            auto *draw = reinterpret_cast<NMTBCUSTOMDRAW *>(b);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                bool hot = (draw->nmcd.uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
                draw->clrText = windowActive || hot ? GetSysColor(COLOR_BTNTEXT) : RGB(128, 128, 128);
                return TBCDRF_USECDCOLORS;
            }
        }
        if (header && header->idFrom >= CaptionBase && header->idFrom < CaptionBase + 3 &&
            header->code == NM_CUSTOMDRAW) {
            auto *draw = reinterpret_cast<NMCUSTOMDRAW *>(b);
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                DRAWITEMSTRUCT item{};
                item.CtlID = (UINT)header->idFrom;
                item.hwndItem = header->hwndFrom;
                item.hDC = draw->hdc;
                item.rcItem = draw->rc;
                item.itemState = ((draw->uItemState & CDIS_SELECTED) ? ODS_SELECTED : 0) |
                                 ((draw->uItemState & CDIS_FOCUS) ? ODS_FOCUS : 0);
                PaintCaptionButton(*this, item);
                return CDRF_SKIPDEFAULT;
            }
        }
        break;
    }
    case WM_NCCALCSIZE:
        if (customFrame && a) {
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(b);
            LONG top = params->rgrc[0].top;
            DefWindowProcW(window, m, a, b);
            UINT currentDpi = GetDpiForWindow(window);
            if (!currentDpi)
                currentDpi = dpi;
            // 保留系统左右及底部框架；最大化时扣除窗口外延，避免内容被屏幕顶端裁切。
            params->rgrc[0].top =
                top + (IsZoomed(window) ? GetSystemMetricsForDpi(SM_CYSIZEFRAME, currentDpi) +
                                              GetSystemMetricsForDpi(SM_CXPADDEDBORDER, currentDpi)
                                        : 0);
            return 0;
        }
        break;
    case WM_NCHITTEST:
        if (customFrame) {
            LRESULT native = DefWindowProcW(window, m, a, b);
            if (native >= HTLEFT && native <= HTBOTTOMRIGHT)
                return native;
            POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
            ScreenToClient(window, &point);
            if (!IsZoomed(window) && point.y >= 0 && point.y < TitleOffset())
                return HTTOP;
            if (point.y >= TitleOffset() && point.y < TitleHeight()) {
                RECT icon{S(4), TitleOffset(), S(32), TitleHeight()};
                if (PtInRect(&icon, point))
                    return HTSYSMENU;
                HWND child = ChildWindowFromPointEx(window, point, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
                if (child && child != window)
                    return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        break;
    case WM_GETMINMAXINFO: {
        // 预留菜单、四个导航按钮和自绘窗口按钮，不把按钮挤入拖动区域。
        auto *limits = reinterpret_cast<MINMAXINFO *>(b);
        DefWindowProcW(window, m, a, b);
        limits->ptMinTrackSize.x = std::max(limits->ptMinTrackSize.x, (LONG)S(500));
        limits->ptMinTrackSize.y = std::max(limits->ptMinTrackSize.y, (LONG)S(240));
        return 0;
    }
    case WM_DWMCOMPOSITIONCHANGED: {
        UpdateFrame();
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        Layout();
        return 0;
    }
    case WM_ACTIVATE:
        windowActive = LOWORD(a) != WA_INACTIVE;
        UpdateFrame();
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, (HBRUSH)(COLOR_BTNFACE + 1));
        if (windowSmallIcon)
            DrawIconEx(dc, S(8), TitleOffset() + S(10), windowSmallIcon, S(16), S(16), 0, nullptr, DI_NORMAL);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MENUSELECT:
        if (menuOpen) {
            selectedMenu = (HMENU)b;
            selectedMenuFlags = HIWORD(a);
        }
        break;
    case WM_SYSCOMMAND:
        if ((a & 0xfff0) == SC_KEYMENU && toolbar) {
            if (b == 0) {
                if (GetFocus() == toolbar) {
                    if (IsWindow(menuReturnFocus))
                        SetFocus(menuReturnFocus);
                } else {
                    menuReturnFocus = GetFocus();
                    SetFocus(toolbar);
                    SendMessageW(toolbar, TB_SETHOTITEM, 0, 0);
                }
                return 0;
            }
        }
        break;
    case WM_DRAWITEM: {
        auto *d = (DRAWITEMSTRUCT *)b;
        if (d->CtlType == ODT_BUTTON && d->CtlID >= CaptionBase && d->CtlID < CaptionBase + 3) {
            PaintCaptionButton(*this, *d);
            return TRUE;
        }
        if (d->CtlType != ODT_BUTTON || d->CtlID < NavigationBase || d->CtlID >= NavigationBase + 8)
            break;
        RECT r = d->rcItem;
        bool disabled = (d->itemState & ODS_DISABLED) != 0;
        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(d->hwndItem, &cursor);
        const int navigationIndex = (d->CtlID - NavigationBase) % 4;
        bool menuPressed = navigationIndex == 2 && quickMenuPane == (int)(d->CtlID - NavigationBase) / 4;
        COLORREF bg = ((d->itemState & ODS_SELECTED) || menuPressed) ? RGB(210, 220, 233)
                      : PtInRect(&r, cursor) && !disabled            ? RGB(228, 234, 242)
                                                                     : GetSysColor(COLOR_WINDOW);
        auto brush = CreateSolidBrush(bg);
        FillRect(d->hDC, &r, brush);
        DeleteObject(brush);
        DrawUiIcon(d->hDC, r, navigationIndex == 2 ? UiIcon::More : static_cast<UiIcon>(navigationIndex), dpi,
                   disabled);
        if (d->itemState & ODS_FOCUS) {
            InflateRect(&r, -S(3), -S(3));
            DrawFocusRect(d->hDC, &r);
        }
        if (auto &bar = addressBars[(d->CtlID - NavigationBase) / 4])
            bar->PaintBorder(d->hDC, d->hwndItem);
        return TRUE;
    }
    case WM_CREATE: {
        dpi = GetDpiForWindow(window);
        RefreshDpiResources();
        BuildMenu();
        toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"菜单",
                                  WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | CCS_NORESIZE |
                                      CCS_NOPARENTALIGN | CCS_NODIVIDER,
                                  0, 0, 1, 1, window, nullptr, instance, nullptr);
        SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(toolbar, WM_SETFONT, (WPARAM)font, TRUE);
        const wchar_t *menus[] = {L"文件", L"查看", L"帮助"};
        for (int i = 0; i < 3; ++i) {
            TBBUTTON button{};
            button.iBitmap = I_IMAGENONE;
            button.idCommand = MenuBase + i;
            button.fsState = TBSTATE_ENABLED;
            button.fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT;
            button.iString = (INT_PTR)menus[i];
            SendMessageW(toolbar, TB_ADDBUTTONSW, 1, (LPARAM)&button);
        }
        for (int i = 0; i < 8; ++i) {
            const wchar_t *labels[] = {L"后退", L"前进", L"快捷访问", L"刷新"};
            HWND btn = CreateWindowW(L"BUTTON", labels[i % 4], WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 1,
                                     1, window, (HMENU)(INT_PTR)(NavigationBase + i), instance, nullptr);
            SendMessageW(btn, WM_SETFONT, (WPARAM)font, TRUE);
            SetWindowSubclass(btn, ButtonHoverProc, 1, 0);
        }
        const wchar_t *captionNames[] = {L"最小化", L"最大化", L"关闭"};
        for (int i = 0; i < 3; ++i) {
            HWND button =
                CreateWindowW(L"BUTTON", captionNames[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                              0, 0, 1, 1, window, (HMENU)(INT_PTR)(CaptionBase + i), instance, nullptr);
            SetWindowSubclass(button, ButtonHoverProc, 1, 0);
            SendMessageW(button, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
        }
        HWND tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0,
                                    0, 0, window, nullptr, instance, nullptr);
        const wchar_t *help[] = {L"后退（Alt+←）", L"前进（Alt+→）", L"快捷访问", L"刷新（F5）"};
        for (int i = 0; i < 8; ++i) {
            TOOLINFOW ti{sizeof(ti)};
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = window;
            ti.uId = (UINT_PTR)GetDlgItem(window, NavigationBase + i);
            ti.lpszText = const_cast<wchar_t *>(help[i % 4]);
            SendMessageW(tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        for (int p = 0; p < 2; ++p) {
            strips[p] = std::make_unique<TabStrip>(*this, p);
            strips[p]->Create();
            addressBars[p] = std::make_unique<AddressBar>(*this, p);
            if (FAILED(addressBars[p]->Create(GetDlgItem(window, NavigationBase + p * 4),
                                              GetDlgItem(window, NavigationBase + p * 4 + 3))))
                return -1;
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
            if (r->item) {
                r->path = RememberLocation(r->item.Get());
                if (r->path.empty())
                    return 0;
            }
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
                ShowError((Id)a, it->second->Navigation().Error());
        }
        Changed();
        if (b == 2)
            RedrawPaneFocus();
        else if (b != 3 && model.PaneOf((Id)a) >= 0)
            Layout(false, model.PaneOf((Id)a));
        UpdateStatus();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(a) >= NavigationBase && LOWORD(a) < NavigationBase + 8) {
            const int offset = LOWORD(a) - NavigationBase;
            Activate(offset / 4);
            if (offset % 4 == 2)
                ShowQuickAccess(offset / 4);
            else
                Command(Back + offset % 4);
        } else if (LOWORD(a) == 500 || LOWORD(a) == 501) {
            int p = LOWORD(a) - 500;
            auto *t = model.Find(model.panes[p].selected);
            if (t) {
                auto view = views.find(t->id);
                if (view != views.end())
                    view->second->Retry();
            }
        } else
            Command(LOWORD(a));
        return 0;
    case WM_SIZE:
        SetWindowTextW(GetDlgItem(window, CaptionBase + 1), IsZoomed(window) ? L"还原" : L"最大化");
        UpdateFrame();
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
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        UpdateFrame();
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
            tasks.Poll();
            for (int p = 0; p < 2; ++p)
                if (GetWindowLongPtrW(AddressEdit(p), GWL_STYLE) & WS_VISIBLE)
                    PostMessageW(AddressEdit(p), WM_APP + 101, 0, 0);
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
    case WM_CLOSE: {
        closing = true;
        Save();
        OleFlushClipboard();
        views.clear();
        for (auto &strip : strips)
            if (strip && IsWindow(strip->hwnd))
                RevokeDragDrop(strip->hwnd);
        MSG pending{};
        while (PeekMessageW(&pending, window, WM_ZEN_NAV, WM_ZEN_NAV, PM_REMOVE))
            delete (NavRequest *)pending.lParam;
        DestroyWindow(window);
        return 0;
    }
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
        for (HWND child : {AddressEdit(0), AddressEdit(1), error[0], error[1], status, toolbar})
            if (child)
                SendMessageW(child, WM_SETFONT, (WPARAM)next, TRUE);
        for (int i = 0; i < 8; ++i)
            if (HWND button = GetDlgItem(window, NavigationBase + i))
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
    for (auto &[id, owned] : homeIcons) {
        auto index = homeIconIndices.find(id);
        HICON icon = index != homeIconIndices.end() ? SystemImageIcon(index->second, S(16)) : nullptr;
        if (!icon)
            icon = CopyIcon(homeIconPaths[id] == Home ? computerIcon : folderIcon);
        if (icon) {
            if (owned)
                DestroyIcon(owned);
            owned = icon;
        }
    }
    Log(L"更新 DPI 资源 dpi=" + std::to_wstring(dpi));
}
static void MoveIfChanged(HWND child, int x, int y, int width, int height, BOOL repaint) {
    RECT current{};
    if (!child || !GetWindowRect(child, &current))
        return;
    MapWindowPoints(nullptr, GetParent(child), (POINT *)&current, 2);
    RECT target{x, y, x + width, y + height};
    if (!EqualRect(&current, &target))
        MoveWindow(child, x, y, width, height, repaint);
}
void App::RedrawPaneFocus() {
    for (int p = 0; p < 2; ++p) {
        InvalidateRect(AddressCrumb(p), nullptr, FALSE);
        for (int i = 0; i < 4; ++i)
            InvalidateRect(GetDlgItem(window, NavigationBase + p * 4 + i), nullptr, FALSE);
    }
}
void App::Layout(bool updateStatus, int pane) {
    if (!status)
        return;
    RECT rc;
    GetClientRect(window, &rc);
    if (pane < 0)
        SendMessageW(status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(status, &sr);
    int bottom = rc.bottom - (sr.bottom - sr.top), top = TitleHeight();
    int titleY = TitleOffset() + (customFrame ? S(2) : 0);
    int titleX = customFrame ? S(32) : 0;
    if (pane < 0) {
        SendMessageW(toolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(S(48), S(32)));
        MoveIfChanged(toolbar, titleX + S(4), titleY, S(144), S(32), TRUE);
        for (int i = 0; i < 3; ++i)
            MoveIfChanged(GetDlgItem(window, CaptionBase + i), rc.right - S((3 - i) * 46), TitleOffset(),
                          S(46), S(36), TRUE);
    }
    int middle = (int)(rc.right * model.split);
    // 窄窗口保留两侧导航按钮及地址输入的基本空间，保存的分栏比例不变。
    const int minimumPane = std::min(S(180), (int)rc.right / 2);
    middle = std::clamp(middle, minimumPane, (int)rc.right - minimumPane);
    for (int p = 0; p < 2; ++p) {
        if (pane >= 0 && pane != p)
            continue;
        int left = p ? middle + S(3) : 0, width = p ? rc.right - left : middle - S(3);
        width = std::max(width, 1);
        int tabHeight = strips[p] ? strips[p]->Arrange(width) : S(32);
        if (strips[p]) {
            MoveIfChanged(strips[p]->hwnd, left, top, width, tabHeight, TRUE);
            InvalidateRect(strips[p]->hwnd, nullptr, FALSE);
        }
        int y = top + tabHeight;
        const int buttonWidth = S(28), addressLeft = left + buttonWidth * 3;
        const int addressWidth = std::max(1, width - buttonWidth * 4);
        for (int i = 0; i < 4; ++i)
            MoveIfChanged(GetDlgItem(window, NavigationBase + p * 4 + i),
                          i == 3 ? left + width - buttonWidth : left + i * buttonWidth, y, buttonWidth, S(32),
                          TRUE);
        MoveIfChanged(AddressCrumb(p), addressLeft, y, addressWidth, S(32), TRUE);
        MoveIfChanged(AddressEdit(p), addressLeft + S(34), y + S(6), std::max(1, addressWidth - S(40)), S(20),
                      TRUE);
        InvalidateRect(AddressCrumb(p), nullptr, TRUE);
        for (int i = 0; i < 4; ++i)
            InvalidateRect(GetDlgItem(window, NavigationBase + p * 4 + i), nullptr, FALSE);
        RECT view{left, y + S(34), left + width, std::max(y + S(35), bottom)};
        viewBounds[p] = view;
        MoveIfChanged(error[p], left + S(12), view.top + S(15), std::max(width - S(24), 1), S(90), TRUE);
        bool failed = false;
        for (auto &[id, v] : views)
            if (model.PaneOf(id) == p) {
                bool selected = id == model.panes[p].selected;
                v->Show(selected && SUCCEEDED(v->Navigation().Error()));
                if (selected) {
                    v->Resize(view);
                    failed = FAILED(v->Navigation().Error());
                }
            }
        if (((GetWindowLongPtrW(error[p], GWL_STYLE) & WS_VISIBLE) != 0) != failed)
            ShowWindow(error[p], failed ? SW_SHOW : SW_HIDE);
        UpdateAddress(p);
    }
    if (updateStatus)
        UpdateStatus();
}
void App::EnsureView(Id id) {
    if (!model.Find(id) || views.contains(id))
        return;
    Layout(false, model.PaneOf(id));
    RefreshLocalIcon(id);
    auto v = std::make_unique<ShellView>(*this, id);
    auto *ptr = v.get();
    views[id] = std::move(v);
    HRESULT hr = ptr->Create(window, viewBounds[model.PaneOf(id)]);
    if (FAILED(hr)) {
        ShowError(id, hr);
        return;
    }
    Layout(false, model.PaneOf(id));
    ptr->Browse({model.Find(id)->path, false});
}
void App::Activate(int p, Id id, bool focus) {
    if (p < 0 || p > 1)
        return;
    if (model.active != p || (id && model.panes[p].selected != id))
        ++pasteGeneration;
    bool paneChanged = model.active != p;
    model.active = p;
    if (id && model.PaneOf(id) == p)
        model.panes[p].selected = id;
    model.EnsureVisibleSelection(p);
    EnsureView(model.panes[p].selected);
    Layout(true, p);
    if (paneChanged)
        RedrawPaneFocus();
    Changed();
    if (focus) {
        auto it = views.find(model.panes[p].selected);
        if (it != views.end() && FAILED(it->second->Navigation().Error())) {
            SetFocus(error[p]);
        } else if (it != views.end() && it->second->shell) {
            HWND w;
            it->second->shell->GetWindow(&w);
            SetFocus(w);
        }
    }
}
void App::Changed() {
    PruneLocations();
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
std::wstring App::AddressText(int p) const {
    return p >= 0 && p < 2 && addressBars[p] ? addressBars[p]->Text() : std::wstring{};
}
void App::UpdateAddress(int p) {
    if (p >= 0 && p < 2 && addressBars[p])
        addressBars[p]->Update();
}
void App::EditAddress(int p) {
    if (p >= 0 && p < 2 && addressBars[p])
        addressBars[p]->Edit();
}
LocationKey App::RememberLocation(IShellItem *item) {
    auto key = model.locations.Remember(item);
    if (key.empty())
        Report(window, L"无法获取目录的 Shell 标识", E_FAIL);
    return key;
}
void App::PruneLocations() {
    std::set<LocationKey> retained(model.recent.begin(), model.recent.end());
    for (const auto &entry : model.quickAccess.links)
        retained.insert(entry.target);
    for (const auto &pane : model.panes)
        for (const auto &tab : pane.tabs) {
            retained.insert(tab.path);
            retained.insert(tab.home);
            retained.insert(tab.history.begin(), tab.history.end());
        }
    for (const auto &[id, view] : views) {
        retained.insert(view->Navigation().Request().destination);
        for (const auto &[label, path] : view->AddressParts())
            retained.insert(path);
    }
    model.locations.Retain(retained);
}
void App::Navigate(Id id, const LocationKey &path, bool append, int historyIndex) {
    ++pasteGeneration;
    auto *t = model.Find(id);
    if (!t || path.empty())
        return;
    EnsureView(id);
    auto it = views.find(id);
    if (it != views.end()) {
        // 位置与历史由视图统一提交：新地址要求成功，已有历史允许停留在失败位置。
        it->second->Capture();
        it->second->Browse({path, append, historyIndex});
        Layout();
    }
}
void App::Travel(int delta) {
    auto *t = model.Find(model.panes[model.active].selected);
    if (!t)
        return;
    int next = t->historyIndex + delta;
    if (next >= 0 && next < (int)t->history.size()) {
        auto path = t->history[next];
        Navigate(t->id, path, false, next);
    }
}
void App::NewTab(int p, const LocationKey &path, Id group, int index, Id opener) {
    HRESULT hr = model.locations.ValidateDirectory(path);
    if (FAILED(hr)) {
        Report(window, L"目标目录无法访问，未新建标签。", hr);
        return;
    }
    Id id = model.Add(p, path, group, index, true, opener);
    if (!id)
        return;
    Activate(p, id);
    strips[p]->EnsureVisible(id);
    BuildMenu();
}
void App::SyncViews(int pane) {
    ResourceTrace trace(L"清理标签资源");
    std::erase_if(views, [&](auto &pair) { return !model.Find(pair.first); });
    PruneLocations();
    std::erase_if(icons, [&](auto &pair) {
        if (model.Find(pair.first))
            return false;
        if (pair.second)
            DestroyIcon(pair.second);
        return true;
    });
    std::erase_if(iconIndices, [&](auto &pair) { return !model.Find(pair.first); });
    std::erase_if(homeIcons, [&](auto &pair) {
        auto *tab = model.Find(pair.first);
        if (tab && !tab->name.empty())
            return false;
        if (pair.second)
            DestroyIcon(pair.second);
        homeIconIndices.erase(pair.first);
        homeIconPaths.erase(pair.first);
        return true;
    });
    for (int p = 0; p < 2; ++p)
        EnsureView(model.panes[p].selected);
    Layout(true, pane);
}
void App::CloseTabs(int p, const std::vector<Id> &ids, bool group) {
    bool named = false;
    for (auto id : ids)
        if (auto *t = model.Find(id))
            named |= !t->name.empty();
    if (named &&
        !Confirm(window, group ? L"当前关闭的组包含主目录标签，是否确认" : L"当前关闭的是主目录标签，是否确认"))
        return;
    model.Remove(p, ids, true, group);
    SyncViews();
    Changed();
    BuildMenu();
}
void App::SetGroupCollapsed(int pane, Id group, bool collapsed, GroupInteraction source) {
    if (pane < 0 || pane > 1 || !model.FindGroup(pane, group))
        return;
    model.SetGroupCollapsed(pane, group, collapsed);
    Changed();
    SyncViews(pane);
    if (source == GroupInteraction::User) {
        Activate(pane);
        strips[pane]->EnsureVisible(model.panes[pane].selected);
    }
}
void App::ToggleGroup(int pane, Id group) {
    if (pane < 0 || pane > 1)
        return;
    if (auto *found = model.FindGroup(pane, group))
        SetGroupCollapsed(pane, group, !found->collapsed);
}
void App::RestoreGroups(const std::vector<GroupExpansionState> &states) {
    if (states.empty())
        return;
    int affected = states.size() == 1 ? states.front().pane : -1;
    for (const auto &state : states)
        model.RestoreGroups(state);
    Changed();
    SyncViews(affected);
}
void App::MoveTab(Id tab, int target, int index, Id group, bool keepExpanded) {
    if (!model.Find(tab) || target < 0 || target > 1 || (group && !model.FindGroup(target, group)))
        return;
    if (keepExpanded)
        model.SetGroupCollapsed(target, group, false);
    model.Move(tab, target, index, group);
    Changed();
    SyncViews();
    Activate(model.active);
}
void App::MoveGroup(int pane, Id group, int target, int index) {
    if (pane < 0 || pane > 1 || target < 0 || target > 1 || !model.FindGroup(pane, group))
        return;
    model.MoveGroup(pane, group, target, index);
    Changed();
    SyncViews();
    Activate(model.active);
}
std::wstring App::Title(const Tab &t) const {
    return !t.name.empty()          ? t.name
           : !t.displayName.empty() ? t.displayName
                                    : model.locations.DisplayName(t.path);
}
HICON App::Icon(Id id) const {
    auto it = icons.find(id);
    if (it != icons.end() && it->second)
        return it->second;
    auto *t = model.Find(id);
    return t && t->path == Home ? computerIcon : folderIcon;
}
HICON App::TabIcon(Id id) const {
    auto *tab = model.Find(id);
    if (!tab || tab->name.empty())
        return Icon(id);
    auto it = homeIcons.find(id);
    if (it != homeIcons.end() && it->second)
        return it->second;
    return tab->home == Home ? computerIcon : folderIcon;
}
void App::RefreshHomeIcon(Id id) {
    auto *tab = model.Find(id);
    if (!tab || tab->name.empty())
        return;
    const auto &key = tab->home;
    auto path = model.locations.FileSystemPath(key);
    // 主目录不变时保留图标，导航元数据仅更新地址栏使用的当前目录图标。
    if (homeIconPaths.contains(id) && homeIconPaths[id] == key && homeIcons[id])
        return;
    HICON icon = nullptr;
    int index = -1;
    if (key == tab->path && icons.contains(id)) {
        icon = CopyIcon(Icon(id));
        if (iconIndices.contains(id))
            index = iconIndices[id];
    } else if (key == Home) {
        SHSTOCKICONINFO info{sizeof(info)};
        if (SUCCEEDED(SHGetStockIconInfo(SIID_DESKTOPPC, SHGSI_SYSICONINDEX, &info)))
            index = info.iSysImageIndex;
    } else {
        const auto state = InspectLocalPath(path);
        if (state == LocalPathState::Exists) {
            SHFILEINFOW info{};
            if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX))
                index = info.iIcon;
        } else if (state == LocalPathState::Missing) {
            SHSTOCKICONINFO info{sizeof(info)};
            if (SUCCEEDED(SHGetStockIconInfo(SIID_ERROR, SHGSI_SYSICONINDEX, &info)))
                index = info.iSysImageIndex;
        }
    }
    if (!icon && index >= 0)
        icon = SystemImageIcon(index, S(16));
    if (!icon)
        icon = CopyIcon(key == Home ? computerIcon : folderIcon);
    if (homeIcons[id])
        DestroyIcon(homeIcons[id]);
    homeIcons[id] = icon;
    homeIconPaths[id] = key;
    homeIconIndices.erase(id);
    if (index >= 0)
        homeIconIndices[id] = index;
}
void App::RefreshLocalIcon(Id id) {
    auto *t = model.Find(id);
    if (!t)
        return;
    HICON icon = nullptr;
    iconIndices.erase(id);
    auto path = model.locations.FileSystemPath(t->path);
    auto state = InspectLocalPath(path);
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
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME)) {
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
    RefreshHomeIcon(id);
}
void App::RefreshMetadata(Id id, IShellItem *item) {
    auto key = model.locations.Remember(item);
    auto title = model.locations.DisplayName(key);
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
    // 延迟加载的主目录取得系统图标后补齐缓存，不让其他目录覆盖它。
    if (auto *tab = model.Find(id); tab && !tab->name.empty() && tab->home == tab->path) {
        homeIconPaths.erase(id);
        RefreshHomeIcon(id);
    }
}
void App::ShowError(Id id, HRESULT hr) {
    RefreshLocalIcon(id);
    auto it = views.find(id);
    if (it != views.end())
        it->second->SetFailure(hr);
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
    for (int p = 0; p < 2; ++p) {
        auto *tab = model.Find(model.panes[p].selected);
        auto view = tab ? views.find(tab->id) : views.end();
        EnableWindow(GetDlgItem(window, NavigationBase + p * 4), tab && tab->historyIndex > 0);
        EnableWindow(GetDlgItem(window, NavigationBase + p * 4 + 1),
                     tab && tab->historyIndex + 1 < (int)tab->history.size());
        EnableWindow(GetDlgItem(window, NavigationBase + p * 4 + 2), tab != nullptr);
        EnableWindow(GetDlgItem(window, NavigationBase + p * 4 + 3), view != views.end());
    }
    auto it = views.find(t->id);
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
    // 菜单正在跟踪时保留其句柄，下次展开时按最新状态重建。
    if (menuOpen)
        return;
    HMENU main = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING | MF_GRAYED, 0, L"最近关闭目录");
    for (size_t i = 0; i < model.recent.size(); ++i)
        AppendMenuW(file, MF_STRING, RecentBase + i, model.locations.DisplayPath(model.recent[i]).c_str());
    if (model.recent.empty())
        AppendMenuW(file, MF_STRING | MF_GRAYED, 0, L"暂无记录");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING | (model.recent.empty() ? MF_GRAYED : 0), ClearRecent, L"清空最近关闭记录");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, Exit, L"退出");
    const wchar_t *modes[] = {L"详细信息", L"列表", L"小图标", L"中等图标", L"大图标", L"超大图标"};
    for (int i = 0; i < 6; ++i)
        AppendMenuW(view, MF_STRING, ViewBase + i, modes[i]);
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING | (model.wrap ? MF_CHECKED : 0), Wrap, L"标签自动换行");
    AppendMenuW(view, MF_STRING | (model.singleExpandedGroup ? MF_CHECKED : 0), SingleGroup,
                L"每栏仅展开一个组");
    HMENU trigger = CreatePopupMenu();
    AppendMenuW(trigger, MF_STRING, GroupSingleClick, L"单击切换");
    AppendMenuW(trigger, MF_STRING, GroupDoubleClick, L"双击切换");
    CheckMenuRadioItem(trigger, GroupSingleClick, GroupDoubleClick,
                       model.groupDoubleClick ? GroupDoubleClick : GroupSingleClick, MF_BYCOMMAND);
    AppendMenuW(view, MF_POPUP, (UINT_PTR)trigger, L"组标题展开/折叠方式");
    AppendMenuW(help, MF_STRING, About, L"关于");
    AppendMenuW(main, MF_POPUP, (UINT_PTR)file, L"文件");
    AppendMenuW(main, MF_POPUP, (UINT_PTR)view, L"查看");
    AppendMenuW(main, MF_POPUP, (UINT_PTR)help, L"帮助");
    auto old = mainMenu;
    mainMenu = main;
    if (old)
        DestroyMenu(old);
}
void App::ShowMainMenu(int index) {
    if (index < 0 || index >= 3 || menuOpen || !toolbar)
        return;
    BuildMenu();
    HWND previous = GetFocus();
    if (previous == toolbar)
        previous = menuReturnFocus;
    menuOpen = true;
    trackingMenu = this;
    HHOOK hook = SetWindowsHookExW(WH_MSGFILTER, MenuFilter, nullptr, GetCurrentThreadId());
    UINT command = 0;
    do {
        menuIndex = index;
        nextMenu = -1;
        selectedMenu = GetSubMenu(mainMenu, index);
        selectedMenuFlags = 0;
        SendMessageW(toolbar, TB_SETHOTITEM, index, 0);
        SendMessageW(toolbar, TB_PRESSBUTTON, MenuBase + index, TRUE);
        RECT rect{};
        SendMessageW(toolbar, TB_GETITEMRECT, index, (LPARAM)&rect);
        MapWindowPoints(toolbar, nullptr, (POINT *)&rect, 2);
        TPMPARAMS params{sizeof(params), rect};
        command = TrackPopupMenuEx(GetSubMenu(mainMenu, index), TPM_RETURNCMD | TPM_LEFTALIGN, rect.left,
                                   rect.bottom, window, &params);
        SendMessageW(toolbar, TB_PRESSBUTTON, MenuBase + index, FALSE);
        index = nextMenu;
    } while (!command && index >= 0 && !closing);
    if (hook)
        UnhookWindowsHookEx(hook);
    trackingMenu = nullptr;
    menuOpen = false;
    SendMessageW(toolbar, TB_SETHOTITEM, (WPARAM)-1, 0);
    if (IsWindow(previous) && GetForegroundWindow() == window)
        SetFocus(previous);
    if (command)
        Command(command);
}
void App::Command(UINT id) {
    if (id == GroupSingleClick || id == GroupDoubleClick) {
        model.groupDoubleClick = id == GroupDoubleClick;
        Changed();
        BuildMenu();
        Log(model.groupDoubleClick ? L"组标题改为双击切换" : L"组标题改为单击切换");
        return;
    }
    if (id >= CaptionBase && id < CaptionBase + 3) {
        WPARAM command = id == CaptionBase       ? SC_MINIMIZE
                         : id == CaptionBase + 2 ? SC_CLOSE
                         : IsZoomed(window)      ? SC_RESTORE
                                                 : SC_MAXIMIZE;
        SendMessageW(window, WM_SYSCOMMAND, command, 0);
        return;
    }
    if (id >= MenuBase && id < MenuBase + 3)
        ShowMainMenu(id - MenuBase);
    else if (id == Back)
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
        if (Confirm(window, L"是否清空最近关闭记录？")) {
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
        AppendMenuW(menu, MF_STRING, 1, t && !t->name.empty() ? L"修改名称…" : L"设置主目录…");
        if (t && !t->name.empty()) {
            AppendMenuW(menu, MF_STRING, 2, L"更新主目录为当前目录");
            AppendMenuW(menu, MF_STRING, 6, L"取消主目录");
        }
        AppendMenuW(menu, MF_STRING, 5, L"在系统资源管理器中打开");
        AppendMenuW(menu, MF_STRING | (model.quickAccess.readOnly ? MF_GRAYED : 0), 7, L"添加到快捷访问…");
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
        auto *current = model.FindGroup(p, group);
        if (current)
            AppendMenuW(menu, MF_STRING, current->collapsed ? 15 : 16,
                        current->collapsed ? L"展开组" : L"折叠组");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 10, L"修改组名");
        HMENU colors = CreatePopupMenu();
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
        if (cmd == 7) {
            AddQuickAccess(tab);
            return;
        }
        if (cmd == 1) {
            auto value =
                Input(window, t->name.empty() ? L"设置主目录" : L"修改名称", Title(*t),
                      t->name.empty() ? L"为标签命名，并将当前目录设为主目录；"
                                        L"双击标签可返回。"
                                      : L"修改名称不会改变主目录；清空名称或选择“取消主目录”可恢复普通标签。");
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
        RefreshHomeIcon(tab);
    } else if (auto *g = model.FindGroup(p, group)) {
        if (cmd == 15 || cmd == 16) {
            // 使用菜单显示时的明确意图，避免菜单打开期间状态变化后反向切换。
            SetGroupCollapsed(p, group, cmd == 16);
            return;
        } else if (cmd == 10) {
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
void App::OpenInExplorer(const LocationKey &path) {
    ComPtr<IShellItem> item;
    HRESULT hr = model.locations.ResolveItem(path, &item);
    SFGAOF attributes = 0;
    if (SUCCEEDED(hr))
        hr = item->GetAttributes(SFGAO_FOLDER, &attributes);
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
    Log(L"系统资源管理器打开 path=" + path.Stored() + L" result=" + std::to_wstring((unsigned long)hr));
    if (FAILED(hr))
        Report(window, L"无法在系统资源管理器中打开：" + model.locations.DisplayPath(path), hr);
}
void App::OpenSelected(ShellView &v, bool newTab) {
    auto items = v.Selected();
    std::vector<LocationKey> paths;
    for (auto &i : items) {
        SFGAOF attr = 0;
        i->GetAttributes(SFGAO_FOLDER, &attr);
        if (!(attr & SFGAO_FOLDER))
            return;
        auto key = RememberLocation(i.Get());
        if (key.empty())
            return;
        paths.push_back(std::move(key));
    }
    if (paths.empty())
        return;
    if (!newTab) {
        Navigate(v.id, paths.front());
        return;
    }
    int p = model.PaneOf(v.id), index = model.Index(p, v.id) + 1;
    Id group = model.Find(v.id)->group, first = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        Id id = model.Add(p, paths[i], group, index++, false, v.id);
        if (!first)
            first = id;
    }
    if (!first)
        return;
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
    PasteFeedback pending{{}, sequence};
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
        tasks.TrackPaste(std::move(pending));
    }
}
void App::DeleteSelection(ShellView &v, bool permanent) {
    std::vector<std::wstring> paths;
    HRESULT hr = v.DeletePaths(paths);
    if (hr == S_FALSE)
        return;
    if (SUCCEEDED(hr)) {
        TaskTicket pending;
        if (LaunchTask(window, {Operation::Delete, L"", paths, permanent}, {}, &pending.result,
                       &pending.process))
            tasks.TrackDelete(std::move(pending));
    } else {
        Log(L"删除项目快照失败 tab=" + std::to_wstring(v.id) + L" " + ErrorText(hr));
        Report(window,
               hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
                   ? L"所选项目不是普通文件或文件夹，暂不支持独立删除。"
                   : L"无法读取待删除项目，操作未开始。",
               hr);
    }
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
    DropFeedback pending;
    if (!LaunchTask(window, task, {}, &pending.result, &pending.process, data, effect))
        return;
    delivered = true;
    Log(L"拖放独立交付 effect=" + std::to_wstring(chosen) + L" destination=" + task.destination);
    for (auto &strip : strips) {
        auto expanded = strip->DetachExternal();
        pending.expanded.insert(pending.expanded.end(), expanded.begin(), expanded.end());
    }
    pending.generation = dropGeneration;
    tasks.TrackDrop(std::move(pending));
    Changed();
}
ShellView *App::MenuSource(Id id, IShellView *shell, const LocationKey &path) {
    auto *tab = model.Find(id);
    auto found = views.find(id);
    if (closing || !IsWindow(window) || !tab || tab->path != path || found == views.end() || !shell ||
        found->second->shell.Get() != shell || found->second->Navigation().Pending())
        return nullptr;
    return found->second.get();
}
void App::FileMenu(ShellView &v, POINT pt, bool background) {
    int pane = model.PaneOf(v.id);
    if (pane < 0 || !v.shell)
        return;
    const Id sourceId = v.id;
    const LocationKey sourcePath = model.Find(sourceId)->path;
    // 扩展和菜单循环均可重入；持有系统视图身份，之后只通过注册表重新取得宿主。
    ComPtr<IShellView> sourceShell = v.shell;
    auto source = [&] { return MenuSource(sourceId, sourceShell.Get(), sourcePath); };
    if (!source())
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
    if (!source())
        return;
    HRESULT hr = session.Create(sourceShell.Get(), background, window,
                                CMF_NORMAL | ((GetKeyState(VK_SHIFT) & 0x8000) ? CMF_EXTENDEDVERBS : 0));
    if (!source())
        return;
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
    if (!source())
        return;
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
    auto *current = source();
    if (!cmd || !current)
        return;
    if (cmd == 0x7001) {
        OpenSelected(*current, true);
        return;
    }
    if (cmd == 0x7002) {
        auto open = [&](IShellItem *item) {
            auto key = RememberLocation(item);
            if (!key.empty())
                OpenInExplorer(key);
        };
        if (selected.empty())
            open(current->Current().Get());
        else if (folders)
            for (auto &item : selected)
                open(item.Get());
        return;
    }
    auto verb = verbOf(cmd);
    current = source();
    if (!current)
        return;
    if (folders && verb == L"open") {
        OpenSelected(*current, false);
        return;
    }
    if (verb == L"copy" || verb == L"cut") {
        CopySelection(*current, verb == L"cut");
        return;
    }
    if (verb == L"paste") {
        // 项目菜单的目标是右键选中的文件夹；快捷键与背景菜单仍使用当前目录。
        if (selected.empty())
            Paste(*current);
        else if (folders && selected.size() == 1)
            Paste(*current, selected.front().Get());
        else
            Report(window, L"请选择一个目标文件夹后粘贴。");
        return;
    }
    if (verb == L"delete") {
        DeleteSelection(*current, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return;
    }
    hr = session.Invoke(cmd, pt);
    if (FAILED(hr) && !closing && IsWindow(window))
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
            model.Find(view.id)->path.Stored());
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
    int caption = GetDlgCtrlID(focus) - (int)CaptionBase;
    bool captionFocus = caption >= 0 && caption < 3 && GetParent(focus) == window;
    if (!alt && !ctrl && !win && (focus == toolbar || captionFocus) && msg.wParam == VK_TAB) {
        SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
        if (focus == toolbar)
            SetFocus(GetDlgItem(window, CaptionBase + (shift ? 2 : 0)));
        else {
            int next = caption + (shift ? -1 : 1);
            if (next < 0 || next >= 3) {
                SetFocus(toolbar);
                SendMessageW(toolbar, TB_SETHOTITEM, 0, 0);
            } else
                SetFocus(GetDlgItem(window, CaptionBase + next));
        }
        return true;
    }
    if (captionFocus && !alt && !ctrl && !win) {
        if (msg.wParam == VK_RETURN) {
            Command(CaptionBase + caption);
            return true;
        }
        if (msg.wParam == VK_ESCAPE) {
            if (IsWindow(menuReturnFocus))
                SetFocus(menuReturnFocus);
            else
                Activate(model.active, 0, true);
            return true;
        }
        // 空格和按钮按压反馈继续由原生 BUTTON 处理。
        if (msg.wParam == VK_SPACE)
            return false;
    }
    if (msg.wParam == VK_F10 && !ctrl && !alt && !shift && !win) {
        SendMessageW(window, WM_SYSCOMMAND, SC_KEYMENU, 0);
        return true;
    }
    if (alt && !ctrl && !shift && !win && (msg.wParam == 'F' || msg.wParam == 'V' || msg.wParam == 'H')) {
        ShowMainMenu(msg.wParam == 'F' ? 0 : msg.wParam == 'V' ? 1 : 2);
        return true;
    }
    if (toolbar && focus == toolbar && !alt && !ctrl && !win) {
        int hot = (int)SendMessageW(toolbar, TB_GETHOTITEM, 0, 0);
        hot = hot < 0 || hot > 2 ? 0 : hot;
        if (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT) {
            SendMessageW(toolbar, TB_SETHOTITEM, (hot + (msg.wParam == VK_LEFT ? 2 : 1)) % 3, 0);
            return true;
        }
        if (msg.wParam == VK_DOWN || msg.wParam == VK_RETURN || msg.wParam == VK_SPACE) {
            ShowMainMenu(hot);
            return true;
        }
        if (msg.wParam == VK_ESCAPE) {
            if (IsWindow(menuReturnFocus))
                SetFocus(menuReturnFocus);
            SendMessageW(toolbar, TB_SETHOTITEM, (WPARAM)-1, 0);
            return true;
        }
    }
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
    if (focus == AddressEdit(0) || focus == AddressEdit(1))
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
