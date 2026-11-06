#include "app.h"
#include <shlobj.h>
namespace zen {
static std::filesystem::path QuickFile() {
    return ConfigDir() / L"quick-access.dat";
}
static std::wstring MenuLabel(const std::wstring &text) {
    std::wstring label;
    for (auto c : text) {
        label += c;
        if (c == L'&')
            label += c;
    }
    return label;
}
bool App::CommitQuickAccess(QuickAccess value) {
    if (model.quickAccess.readOnly || !value.Save(model.locations, QuickFile())) {
        Report(window, L"无法保存快捷访问，请检查文件状态或目录权限。");
        return false;
    }
    model.quickAccess = std::move(value);
    Changed();
    PruneLocations();
    return true;
}
void App::AddQuickAccess(Id id) {
    auto *tab = model.Find(id);
    if (!tab || model.quickAccess.readOnly)
        return;
    const auto target = tab->path;
    auto identity = model.locations.EnsureId(target);
    std::wstring name = model.locations.DisplayName(target);
    bool existing = false;
    for (const auto &entry : model.quickAccess.links)
        if (model.locations.EnsureId(entry.target) == identity ||
            (model.locations.Pidl(target) &&
             model.locations.Same(entry.target, model.locations.Pidl(target)))) {
            name = entry.name;
            existing = true;
            break;
        }
    auto edited = Input(window, existing ? L"修改快捷访问名称" : L"添加到快捷访问", name,
                        L"保存当前实际目录，左右栏共用。名称不会改变目标目录。");
    if (!edited || closing)
        return;
    if (edited->empty()) {
        Report(window, L"请输入快捷访问名称。");
        return;
    }
    auto next = model.quickAccess;
    if (next.Put(model.locations, target, *edited))
        CommitQuickAccess(std::move(next));
}
void App::ShowQuickAccess(int pane) {
    if (pane < 0 || pane > 1 || quickMenuPane >= 0)
        return;
    Activate(pane);
    const Id tab = model.panes[pane].selected;
    const auto links = model.quickAccess.links;
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, 1, L"此电脑");
    AppendMenuW(menu, MF_STRING, 2, L"桌面");
    AppendMenuW(menu, MF_STRING, 3, L"用户目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (model.quickAccess.readOnly ? MF_GRAYED : 0), 4, L"添加到快捷访问…");
    AppendMenuW(menu, MF_STRING | (model.quickAccess.readOnly ? MF_GRAYED : 0), 5, L"管理快捷访问…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (links.empty())
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"暂无快捷访问");
    for (size_t i = 0; i < links.size(); ++i)
        AppendMenuW(menu, MF_STRING, 100 + i, MenuLabel(links[i].name).c_str());
    HWND button = GetDlgItem(window, 130 + pane * 4 + 2);
    RECT bounds{};
    GetWindowRect(button, &bounds);
    quickMenuPane = pane;
    InvalidateRect(button, nullptr, FALSE);
    UINT command =
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY, bounds.left, bounds.bottom, window, nullptr);
    DestroyMenu(menu);
    quickMenuPane = -1;
    if (IsWindow(button))
        InvalidateRect(button, nullptr, FALSE);
    if (closing || !command)
        return;
    if (command == 5) {
        ManageQuickAccess();
        return;
    }
    if (!model.Find(tab) || model.panes[pane].selected != tab)
        return;
    if (command == 4) {
        AddQuickAccess(tab);
        return;
    }
    LocationKey target;
    if (command >= 100 && command - 100 < links.size())
        target = links[command - 100].target;
    else if (command >= 1 && command <= 3) {
        const KNOWNFOLDERID *folders[] = {&FOLDERID_ComputerFolder, &FOLDERID_Desktop, &FOLDERID_Profile};
        PIDLIST_ABSOLUTE pidl = nullptr;
        ComPtr<IShellItem> item;
        HRESULT hr = SHGetKnownFolderIDList(*folders[command - 1], 0, nullptr, &pidl);
        if (SUCCEEDED(hr))
            hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
        CoTaskMemFree(pidl);
        if (FAILED(hr)) {
            Report(window, L"无法打开系统位置。", hr);
            return;
        }
        target = model.locations.Remember(item.Get());
    }
    if (!target.empty() && !closing && model.Find(tab) && model.panes[pane].selected == tab)
        Navigate(tab, target);
}
struct QuickManager {
    App &app;
    QuickAccess draft;
    HWND list{}, path{};
    HFONT font{};
    UINT dpi = 96;
    bool done = false;
    int S(int x) const {
        return MulDiv(x, dpi, 96);
    }
    int Selected() const {
        return (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    }
    void Selection(HWND window) {
        int i = Selected();
        bool valid = i >= 0 && i < (int)draft.links.size();
        SetWindowTextW(path, valid ? app.model.locations.DisplayPath(draft.links[i].target).c_str() : L"");
        for (int id : {301, 302, 305})
            EnableWindow(GetDlgItem(window, id), valid);
        EnableWindow(GetDlgItem(window, 303), valid && i > 0);
        EnableWindow(GetDlgItem(window, 304), valid && i + 1 < (int)draft.links.size());
    }
    void Populate(HWND window, int selected) {
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto &entry : draft.links)
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)entry.name.c_str());
        if (!draft.links.empty())
            SendMessageW(list, LB_SETCURSEL, std::clamp(selected, 0, (int)draft.links.size() - 1), 0);
        Selection(window);
    }
    void Layout(HWND window) {
        RECT r{};
        GetClientRect(window, &r);
        MoveWindow(list, S(12), S(12), r.right - S(24), r.bottom - S(126), TRUE);
        MoveWindow(path, S(12), r.bottom - S(105), r.right - S(24), S(24), TRUE);
        for (int i = 0; i < 5; ++i)
            MoveWindow(GetDlgItem(window, 301 + i), S(12 + i * 105), r.bottom - S(74), S(98), S(28), TRUE);
        MoveWindow(GetDlgItem(window, IDOK), r.right - S(188), r.bottom - S(36), S(80), S(26), TRUE);
        MoveWindow(GetDlgItem(window, IDCANCEL), r.right - S(96), r.bottom - S(36), S(80), S(26), TRUE);
    }
    void Font(HWND window) {
        if (font)
            DeleteObject(font);
        font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    }
};
static LRESULT CALLBACK ManagerProc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto *p = (QuickManager *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        p = (QuickManager *)((CREATESTRUCTW *)b)->lpCreateParams;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)p);
    }
    if (!p)
        return DefWindowProcW(w, m, a, b);
    if (m == WM_CREATE) {
        auto create = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
            return CreateWindowExW(cls == std::wstring(L"LISTBOX") ? WS_EX_CLIENTEDGE : 0, cls, text,
                                   WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, w, (HMENU)(INT_PTR)id,
                                   p->app.instance, nullptr);
        };
        p->list = create(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 300);
        p->path = create(L"STATIC", L"", SS_PATHELLIPSIS, 310);
        const wchar_t *labels[] = {L"修改名称…", L"修改目录…", L"上移", L"下移", L"移除"};
        for (int i = 0; i < 5; ++i)
            create(L"BUTTON", labels[i], WS_TABSTOP, 301 + i);
        create(L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
        create(L"BUTTON", L"取消", WS_TABSTOP, IDCANCEL);
        p->Font(w);
        p->Layout(w);
        p->Populate(w, 0);
        SetFocus(p->list);
        return 0;
    }
    if (m == WM_DPICHANGED) {
        p->dpi = HIWORD(a);
        p->Font(w);
        auto *r = (RECT *)b;
        SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        p->Layout(w);
        return 0;
    }
    if (m == WM_SIZE) {
        p->Layout(w);
        return 0;
    }
    if (m == WM_CLOSE) {
        p->done = true;
        return 0;
    }
    if (m == WM_COMMAND) {
        int command = LOWORD(a), i = p->Selected();
        if (command == IDOK) {
            if (p->app.CommitQuickAccess(p->draft)) {
                p->done = true;
            }
            return 0;
        }
        if (command == IDCANCEL) {
            p->done = true;
            return 0;
        }
        if (command == 300) {
            p->Selection(w);
            return 0;
        }
        if (i < 0 || i >= (int)p->draft.links.size())
            return 0;
        if (command == 301) {
            auto name = Input(w, L"修改快捷访问名称", p->draft.links[i].name);
            if (name && !name->empty())
                p->draft.links[i].name = *name;
        } else if (command == 302) {
            auto text =
                Input(w, L"修改快捷访问目录", p->app.model.locations.EditText(p->draft.links[i].target),
                      L"只修改此快捷访问，不改变标签、主目录或历史。");
            if (text) {
                LocationKey target;
                HRESULT hr = p->app.model.locations.ResolveInput(*text, target);
                if (FAILED(hr))
                    Report(w, L"无法确定目标目录。", hr);
                else {
                    auto others = p->draft;
                    others.Remove(i);
                    auto count = others.links.size();
                    others.Put(p->app.model.locations, target, L"检查重复");
                    if (others.links.size() == count)
                        Report(w, L"此目录已在快捷访问中。");
                    else
                        p->draft.links[i].target = target;
                }
            }
        } else if (command == 303 && p->draft.Move(i, -1))
            --i;
        else if (command == 304 && p->draft.Move(i, 1))
            ++i;
        else if (command == 305)
            p->draft.Remove(i);
        p->Populate(w, i);
        return 0;
    }
    return DefWindowProcW(w, m, a, b);
}
void App::ManageQuickAccess() {
    if (model.quickAccess.readOnly)
        return;
    QuickManager state{*this, model.quickAccess};
    state.dpi = dpi;
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = ManagerProc;
    wc.lpszClassName = L"ZenQuickAccess";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    RECT parent{}, bounds{0, 0, S(550), S(360)};
    GetWindowRect(window, &parent);
    AdjustWindowRectExForDpi(&bounds, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE, WS_EX_DLGMODALFRAME, dpi);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"管理快捷访问",
                                  WS_CAPTION | WS_SYSMENU | WS_POPUP, parent.left + S(40), parent.top + S(40),
                                  bounds.right - bounds.left, bounds.bottom - bounds.top, window, nullptr,
                                  instance, &state);
    if (!dialog)
        return;
    EnableWindow(window, FALSE);
    ShowWindow(dialog, SW_SHOW);
    MSG msg{};
    while (!state.done) {
        int result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            if (!result)
                PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(dialog);
    if (state.font)
        DeleteObject(state.font);
    PruneLocations();
    if (IsWindow(window)) {
        EnableWindow(window, TRUE);
        SetForegroundWindow(window);
    }
}
} // namespace zen
