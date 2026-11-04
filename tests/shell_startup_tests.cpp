#include "app.h"
#include "shell_view.h"
#include "tabs.h"
#include "shell_menu.h"
#include <gdiplus.h>
#include <iostream>
#include <fstream>
using namespace zen;
struct CheckState {
    App *app;
    Id original, background, cached;
    int stage = 0;
    ULONGLONG since = GetTickCount64();
    bool failed = false, busy = false;
};
static CheckState *state;
static LRESULT CALLBACK CountLayoutMessages(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR,
                                            DWORD_PTR data) {
    if (message == WM_WINDOWPOSCHANGED || message == WM_SIZE || message == WM_SHOWWINDOW)
        ++*reinterpret_cast<int *>(data);
    return DefSubclassProc(window, message, w, l);
}
static bool Ready(Id id) {
    auto &app = *state->app;
    auto it = app.views.find(id);
    if (it == app.views.end() || it->second->navigating || !it->second->shell)
        return false;
    auto &view = *it->second;
    ComPtr<IFolderView2> folder;
    int count = 0;
    HWND hwnd = nullptr;
    RECT bounds{}, hostBounds{};
    GetClientRect(view.host, &hostBounds);
    return SUCCEEDED(view.shell.As(&folder)) && SUCCEEDED(folder->ItemCount(SVGIO_ALLVIEW, &count)) &&
           count > 0 && SUCCEEDED(view.shell->GetWindow(&hwnd)) && GetClientRect(hwnd, &bounds) &&
           bounds.right > 10 && bounds.bottom > 10 && bounds.right == hostBounds.right &&
           bounds.bottom == hostBounds.bottom && (GetWindowLongPtrW(view.host, GWL_STYLE) & WS_VISIBLE);
}
static void CALLBACK Check(HWND, UINT, UINT_PTR, DWORD) {
    if (!state || state->busy || !state->app->window || state->stage >= 6)
        return;
    state->busy = true;
    auto &app = *state->app;
    if (app.model.Find(state->cached) && (app.Title(*app.model.Find(state->cached)) != L"离线目录显示名称" ||
                                          app.views.contains(state->cached))) {
        state->failed = true;
        PostMessageW(app.window, WM_CLOSE, 0, 0);
        state->busy = false;
        return;
    }
    if (GetTickCount64() - state->since > 15000) {
        std::cerr << "shell stage timeout: " << state->stage << "\n";
        for (auto &[id, v] : app.views) {
            int count = -1;
            ComPtr<IFolderView2> f;
            if (v->shell && SUCCEEDED(v->shell.As(&f)))
                f->ItemCount(SVGIO_ALLVIEW, &count);
            HWND child = nullptr;
            RECT cr{}, hr{};
            if (v->shell)
                v->shell->GetWindow(&child);
            GetClientRect(child, &cr);
            GetClientRect(v->host, &hr);
            std::cerr << "child=" << child << " child rect=" << cr.right << "," << cr.bottom
                      << " host rect=" << hr.right << "," << hr.bottom << " style=" << std::hex
                      << GetWindowLongPtrW(v->host, GWL_STYLE) << std::dec << "\n";
            std::cerr << "tab " << id << " navigating=" << v->navigating << " shell=" << (bool)v->shell
                      << " count=" << count << "\n";
        }
        state->failed = true;
        PostMessageW(app.window, WM_CLOSE, 0, 0);
    } else if (Ready(app.model.panes[0].selected) && Ready(app.model.panes[1].selected)) {
        std::cout << "stage " << state->stage << " both panes populated\n";
        auto id = state->original;
        auto *tab = app.model.Find(id);
        auto checkMode = [&](FOLDERVIEWMODE expected) {
            ComPtr<IFolderView2> folder;
            FOLDERVIEWMODE actual{};
            int size = 0;
            if (FAILED(app.views.at(id)->shell.As(&folder)) ||
                FAILED(folder->GetViewModeAndIconSize(&actual, &size)) || actual != expected ||
                tab->viewMode != (UINT)expected) {
                std::cerr << "view mode mismatch at stage " << state->stage << "\n";
                state->failed = true;
            }
        };
        auto setIcons = [&] {
            tab->viewMode = FVM_ICON;
            tab->iconSize = 48;
            app.views.at(id)->Apply();
        };
        if (state->stage == 0) {
            checkMode(FVM_ICON);
            // 常用标签图标与当前目录图标分离，导航不能覆盖主目录图标。
            Id favorite = app.model.Add(0, Home, 0, -1, false);
            app.model.Rename(favorite, L"常用测试");
            app.RefreshLocalIcon(favorite);
            HICON homeIcon = app.TabIcon(favorite);
            app.model.Navigate(favorite, DataDir().wstring());
            app.RefreshLocalIcon(favorite);
            auto current = ParseItem(DataDir().wstring());
            app.RefreshMetadata(favorite, current.Get());
            if (!homeIcon || app.TabIcon(favorite) != homeIcon ||
                app.TabIcon(favorite) == app.Icon(favorite) ||
                app.Title(*app.model.Find(favorite)) != L"常用测试")
                state->failed = true;
            app.model.Find(favorite)->home = DataDir().wstring();
            app.RefreshHomeIcon(favorite);
            if (app.homeIconPaths[favorite] != DataDir().wstring())
                state->failed = true;
            app.model.Rename(favorite, L"");
            app.SyncViews();
            if (app.TabIcon(favorite) != app.Icon(favorite) || app.homeIcons.contains(favorite) ||
                app.homeIconIndices.contains(favorite) || app.homeIconPaths.contains(favorite))
                state->failed = true;
            app.model.Rename(favorite, L"关闭缓存测试");
            app.RefreshHomeIcon(favorite);
            app.model.Remove(0, {favorite}, false);
            app.SyncViews();
            if (app.homeIcons.contains(favorite) || app.homeIconIndices.contains(favorite) ||
                app.homeIconPaths.contains(favorite))
                state->failed = true;
            auto recent = app.model.recent;
            app.model.recent = {Home};
            app.BuildMenu();
            wchar_t recentLabel[128]{};
            GetMenuStringW(GetSubMenu(app.mainMenu, 0), 1, recentLabel, 128, MF_BYPOSITION);
            if (std::wstring(recentLabel) != L"此电脑" || app.model.recent.front() != Home)
                state->failed = true;
            app.model.recent = std::move(recent);
            app.BuildMenu();
            // 菜单留在标题行，导航分别位于两栏地址区。
            RECT menuBounds{}, tabBounds{};
            GetWindowRect(app.toolbar, &menuBounds);
            GetWindowRect(app.strips[0]->hwnd, &tabBounds);
            if (GetMenu(app.window) || !IsMenu(app.mainMenu) || GetMenuItemCount(app.mainMenu) != 3 ||
                SendMessageW(app.toolbar, TB_BUTTONCOUNT, 0, 0) != 3 || menuBounds.bottom > tabBounds.top) {
                std::cerr << "combined menu and navigation layout mismatch\n";
                state->failed = true;
            }
            int previousPane = app.model.active;
            HWND navigationFocus = GetFocus();
            for (int p = 0; p < 2; ++p) {
                RECT back{}, up{}, address{}, refresh{};
                GetWindowRect(GetDlgItem(app.window, 130 + p * 4), &back);
                GetWindowRect(GetDlgItem(app.window, 132 + p * 4), &up);
                GetWindowRect(app.crumb[p], &address);
                GetWindowRect(GetDlgItem(app.window, 133 + p * 4), &refresh);
                if (back.top != address.top || refresh.top != address.top || up.right != address.left ||
                    address.right != refresh.left || IsWindowEnabled(GetDlgItem(app.window, 132 + p * 4))) {
                    std::cerr << "pane navigation layout/state mismatch pane=" << p << "\n";
                    state->failed = true;
                }
                app.Activate(1 - p);
                // 以按钮通知验证路由；此电脑的上一级无导航副作用。
                SendMessageW(app.window, WM_COMMAND, MAKEWPARAM(132 + p * 4, BN_CLICKED),
                             (LPARAM)GetDlgItem(app.window, 132 + p * 4));
                if (app.model.active != p) {
                    std::cerr << "pane navigation routing mismatch\n";
                    state->failed = true;
                }
                app.EditAddress(p);
                RECT editor{};
                GetWindowRect(app.address[p], &editor);
                if (editor.right > refresh.left ||
                    !(GetWindowLongPtrW(GetDlgItem(app.window, 133 + p * 4), GWL_STYLE) & WS_VISIBLE)) {
                    std::cerr << "address editor overlapped or hid refresh\n";
                    state->failed = true;
                }
                SendMessageW(app.address[p], WM_KEYDOWN, VK_ESCAPE, 0);
            }
            SetFocus(navigationFocus);
            app.Activate(previousPane);
            // 右栏分组切换及动画结束，不应重新移动或显示左栏 Shell 宿主。
            Id probeTab = app.model.Add(1, Home, 0, -1, false);
            Id probeGroup = app.model.MakeGroup(1, probeTab);
            app.SyncViews(1);
            auto &leftView = *app.views.at(app.model.panes[0].selected);
            int leftLayoutMessages = 0;
            SetWindowSubclass(leftView.host, CountLayoutMessages, 99, (DWORD_PTR)&leftLayoutMessages);
            ValidateRect(app.strips[0]->hwnd, nullptr);
            for (bool collapse : {true, false}) {
                app.model.SetGroupCollapsed(1, probeGroup, collapse);
                app.SyncViews(1);
                app.Activate(1);
                Sleep(230);
                SendMessageW(app.strips[1]->hwnd, WM_TIMER, 2, 0);
            }
            leftView.Resize(app.viewBounds[0]);
            leftView.Show(true);
            if (leftLayoutMessages || GetUpdateRect(app.strips[0]->hwnd, nullptr, FALSE)) {
                std::cerr << "right group operation relaid out left pane\n";
                state->failed = true;
            }
            RemoveWindowSubclass(leftView.host, CountLayoutMessages, 99);
            app.model.Remove(1, {probeTab}, false);
            app.SyncViews(1);
            SetFocus(navigationFocus);
            app.Activate(previousPane);
            if (app.customFrame) {
                auto hit = [&](int x, int y) {
                    POINT point{x, y};
                    ClientToScreen(app.window, &point);
                    return SendMessageW(app.window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
                };
                // 标题空白、菜单控件、系统图标和上边缘各自保留正确的原生命中语义。
                int titleCenter = app.TitleOffset() + app.S(18);
                if (hit(app.S(350), titleCenter) != HTCAPTION || hit(app.S(50), titleCenter) != HTCLIENT ||
                    hit(app.S(16), titleCenter) != HTSYSMENU || hit(app.S(350), 1) != HTTOP) {
                    std::cerr << "integrated title hit testing mismatch\n";
                    state->failed = true;
                }
                POINT origin{};
                ClientToScreen(app.window, &origin);
                if (tabBounds.top - origin.y != app.TitleHeight()) {
                    std::cerr << "integrated title and tabs layout mismatch\n";
                    state->failed = true;
                }
            }
            int menuPane = app.model.active;
            HWND previous = GetFocus();
            SendMessageW(app.window, WM_SYSCOMMAND, SC_KEYMENU, 0);
            MSG menuKey{};
            menuKey.hwnd = app.toolbar;
            menuKey.message = WM_KEYDOWN;
            menuKey.wParam = VK_RIGHT;
            if (GetFocus() != app.toolbar || !app.Key(menuKey) ||
                SendMessageW(app.toolbar, TB_GETHOTITEM, 0, 0) != 1)
                state->failed = true;
            menuKey.wParam = VK_ESCAPE;
            if (!app.Key(menuKey) || (IsWindow(previous) && GetFocus() != previous) ||
                app.model.active != menuPane)
                state->failed = true;
            SendMessageW(app.window, WM_SYSCOMMAND, SC_KEYMENU, 0);
            menuKey.wParam = VK_TAB;
            const wchar_t *captionNames[] = {L"最小化", L"最大化", L"关闭"};
            for (int i = 0; i < 3; ++i) {
                HWND button = GetDlgItem(app.window, 123 + i);
                if (!app.Key(menuKey) || GetFocus() != button || WindowText(button) != captionNames[i])
                    state->failed = true;
                if (SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)
                    state->failed = true;
                // 只按下后取消，不执行最小化、最大化或关闭命令。
                SendMessageW(button, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(4, 4));
                SendMessageW(button, WM_CANCELMODE, 0, 0);
                if (!(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
                    state->failed = true;
                RECT bounds{};
                GetWindowRect(button, &bounds);
                POINT point{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
                if (SendMessageW(app.window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) != HTCLIENT)
                    state->failed = true;
            }
            menuKey.wParam = VK_ESCAPE;
            if (!app.Key(menuKey) || (IsWindow(previous) && GetFocus() != previous))
                state->failed = true;
            // 隔离窗口内验证失焦取消编辑，不提交文字，也不切换活动栏。
            auto addressId = app.model.panes[0].selected;
            auto path = app.model.Find(addressId)->path;
            auto editText = path == Home ? L"" : path;
            app.EditAddress(0);
            SetWindowTextW(app.address[0], L"C:/unsubmitted-address");
            int active = app.model.active;
            if (!app.addressComplete[0] || !app.addressDropDown[0] ||
                !(GetWindowLongPtrW(app.crumb[0], GWL_STYLE) & WS_VISIBLE))
                state->failed = true;
            SetFocus(app.address[1]);
            SendMessageW(app.address[0], WM_APP + 101, 0, 0);
            active = app.model.active;
            if ((GetWindowLongPtrW(app.address[0], GWL_STYLE) & WS_VISIBLE) ||
                !(GetWindowLongPtrW(app.crumb[0], GWL_STYLE) & WS_VISIBLE) ||
                app.model.Find(addressId)->path != path || WindowText(app.address[0]) != editText ||
                app.model.active != active) {
                std::cerr << "address blur daddressId not cancel editing correctly\n";
                state->failed = true;
            }
            app.EditAddress(0);
            if (WindowText(app.address[0]) != editText)
                state->failed = true;
            SendMessageW(app.address[0], WM_KEYDOWN, VK_ESCAPE, 0);
            app.Activate(1, state->background);
        } else if (state->stage == 1) {
            app.Activate(1, state->original);
        } else if (state->stage == 2) {
            checkMode(FVM_ICON);
            setIcons();
            app.Navigate(id, tab->path, false);
        } else if (state->stage == 3) {
            checkMode(FVM_ICON);
            app.Navigate(id, DataDir().wstring());
            app.Navigate(app.model.panes[0].selected, DataDir().wstring());
        } else if (state->stage == 4) {
            checkMode(FVM_DETAILS);
            setIcons();
            app.EditAddress(0);
            if (WindowText(app.address[0]) != DataDir().wstring())
                state->failed = true;
            SetWindowTextW(app.address[0], L"");
            SendMessageW(app.address[0], WM_KEYDOWN, VK_RETURN, 0);
            app.Activate(1, id);
            app.Travel(-1);
        } else {
            if (app.model.Find(app.model.panes[0].selected)->path != Home)
                state->failed = true;
            checkMode(FVM_DETAILS);
            setIcons();
            // 完整鼠标消息序列：组单击不切换，双击只切换一次，展开保留当前选择。
            Id leftBefore = app.model.panes[0].selected;
            bool previousTrigger = app.model.groupDoubleClick;
            app.Command(116);
            int activeBefore = app.model.active;
            Id grouped = app.model.Add(0, Home, 0, -1, false);
            Id groupId = app.model.MakeGroup(0, grouped);
            app.Layout();
            auto groupPoint = [&]() {
                for (auto &hit : app.strips[0]->hits)
                    if (!hit.tab && hit.group == groupId)
                        return MAKELPARAM((hit.rect.left + hit.rect.right) / 2,
                                          (hit.rect.top + hit.rect.bottom) / 2);
                return (LPARAM)0;
            };
            auto clickGroup = [&](bool twice) {
                LPARAM point = groupPoint();
                SendMessageW(app.strips[0]->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(app.strips[0]->hwnd, WM_LBUTTONUP, 0, point);
                if (twice) {
                    SendMessageW(app.strips[0]->hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
                    SendMessageW(app.strips[0]->hwnd, WM_LBUTTONUP, 0, point);
                }
            };
            clickGroup(false);
            if (app.model.active != 0 || app.model.panes[0].selected != leftBefore ||
                app.model.FindGroup(0, groupId)->collapsed) {
                std::cerr << "group single click changed selection or expansion\n";
                state->failed = true;
            }
            app.Activate(0, grouped);
            clickGroup(true);
            if (!app.model.FindGroup(0, groupId)->collapsed || app.model.panes[0].selected == grouped) {
                std::cerr << "group double click did not collapse exactly once\n";
                state->failed = true;
            }
            clickGroup(false);
            if (!app.model.FindGroup(0, groupId)->collapsed)
                state->failed = true;
            clickGroup(true);
            if (app.model.FindGroup(0, groupId)->collapsed || app.model.panes[0].selected != leftBefore) {
                std::cerr << "group expansion changed current selection\n";
                state->failed = true;
            }
            app.Command(115);
            clickGroup(true);
            if (!app.model.FindGroup(0, groupId)->collapsed) {
                std::cerr << "single click mode double click toggled twice\n";
                state->failed = true;
            }
            clickGroup(false);
            if (app.model.FindGroup(0, groupId)->collapsed || app.model.panes[0].selected != leftBefore)
                state->failed = true;
            app.Command(previousTrigger ? 116 : 115);
            // 真实标签布局：折叠立即改变选择，残留动画图像不可命中；反向从当前宽度继续。
            auto &strip = *app.strips[0];
            RECT stripBounds{};
            GetClientRect(strip.hwnd, &stripBounds);
            Sleep(230);
            strip.Arrange(stripBounds.right);
            auto widthOf = [&] {
                for (const auto &hit : strip.hits)
                    if (hit.tab == grouped)
                        return hit.rect.right - hit.rect.left;
                return 0L;
            };
            LONG fullWidth = widthOf();
            app.Activate(0, grouped);
            app.model.SetGroupCollapsed(0, groupId, true);
            app.Layout();
            if (!strip.IsAnimating() || app.model.panes[0].selected == grouped)
                state->failed = true;
            for (const auto &hit : strip.hits)
                if (hit.tab == grouped && strip.Hit({hit.rect.left + 1, hit.rect.top + 1}).tab == grouped)
                    state->failed = true;
            Sleep(45);
            SendMessageW(strip.hwnd, WM_TIMER, 2, 0);
            LONG shrinkingWidth = widthOf();
            if (shrinkingWidth <= 0 || shrinkingWidth >= fullWidth)
                state->failed = true;
            for (const auto &hit : strip.hits)
                if (hit.tab == grouped && hit.naturalWidth != fullWidth)
                    state->failed = true;
            app.model.SetGroupCollapsed(0, groupId, false);
            app.Layout();
            if (!strip.IsAnimating() || widthOf() > shrinkingWidth ||
                app.model.panes[0].selected != leftBefore)
                state->failed = true;
            Sleep(230);
            SendMessageW(strip.hwnd, WM_TIMER, 2, 0);
            if (strip.IsAnimating() || widthOf() != fullWidth)
                state->failed = true;
            app.model.SetGroupCollapsed(0, groupId, true);
            app.Layout();
            strip.Arrange(stripBounds.right - 1);
            if (strip.IsAnimating() || widthOf())
                state->failed = true;
            app.model.SetGroupCollapsed(0, groupId, false);
            app.Layout();
            app.CloseTabs(0, {grouped}, true);
            app.Activate(0, leftBefore);
            app.Activate(activeBefore);
            // 关闭单标签与整组后，图标缓存必须回到原大小。
            size_t iconsBefore = app.icons.size(), indicesBefore = app.iconIndices.size();
            auto &view = *app.views.at(id);
            view.Hook();
            ULONG refsBefore = view.AddRef();
            view.Release();
            DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
            for (int cycle = 0; cycle < 32; ++cycle) {
                Id first = app.model.Add(0, DataDir().wstring(), 0, -1, false);
                Id second = app.model.Add(0, Home, 0, -1, false);
                app.RefreshLocalIcon(first);
                app.RefreshLocalIcon(second);
                if (cycle % 2) {
                    Id group = app.model.MakeGroup(0, first);
                    app.model.AssignGroup(second, group);
                    app.CloseTabs(0, {first, second}, true);
                } else {
                    app.CloseTabs(0, {first});
                    app.CloseTabs(0, {second});
                }
                // 模拟 Shell 导航销毁旧文件子窗口，验证 OLE 注册引用配对释放。
                HWND child = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD, 0, 0, 10, 10, view.host, nullptr,
                                           GetModuleHandleW(nullptr), nullptr);
                if (!child)
                    state->failed = true;
                view.Hook();
                DestroyWindow(child);
                ShellMenu menu;
                if (FAILED(menu.Create(view.shell.Get(), true, app.window)))
                    state->failed = true;
            }
            ULONG refsAfter = view.AddRef();
            view.Release();
            if (app.icons.size() != iconsBefore || app.iconIndices.size() != indicesBefore ||
                refsBefore != refsAfter) {
                std::cerr << "resource lifecycle mismatch icons/indices/refs\n";
                state->failed = true;
            }
            std::cout << "resource cycles=32 user before=" << userBefore
                      << " after=" << GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)
                      << " refs=" << refsAfter << "\n";
            PostMessageW(app.window, WM_CLOSE, 0, 0);
        }
        ++state->stage;
        state->since = GetTickCount64();
    }
    state->busy = false;
}
int main() {
    auto dir = std::filesystem::current_path() / L".test-data" /
               (L"shell-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", dir.c_str());
    Model model;
    Id original = model.panes[1].selected;
    model.Find(original)->viewMode = FVM_ICON;
    model.Find(original)->iconSize = 48;
    Id background = model.Add(1, Home, 0, -1, false);
    Id cached = model.Add(1, (dir / L"not-present").wstring(), 0, -1, false);
    model.Find(cached)->displayName = L"离线目录显示名称";
    if (!SaveModel(model, ConfigDir() / L"session.dat"))
        return 1;
    if (FAILED(OleInitialize(nullptr)))
        return 1;
    // 使用系统枚举器确认本地目录可提供补全候选。
    std::filesystem::create_directories(dir / L"candidate-folder");
    auto childrenRoot = dir / L"breadcrumb-children";
    std::filesystem::create_directories(childrenRoot / L"folder10");
    std::filesystem::create_directories(childrenRoot / L"folder2");
    std::ofstream(childrenRoot / L"file.txt") << "not a folder";
    std::vector<std::pair<std::wstring, std::wstring>> children;
    if (FAILED(ChildFolders(childrenRoot.wstring(), children)) || children.size() != 2 ||
        children[0].first != L"folder2" || children[1].first != L"folder10" ||
        children[0].second != (childrenRoot / L"folder2").wstring())
        return 1;
    if (FAILED(ChildFolders((childrenRoot / L"folder2").wstring(), children)) || !children.empty())
        return 1;
    if (SUCCEEDED(ChildFolders((childrenRoot / L"missing").wstring(), children)) || !children.empty())
        return 1;
    if (FAILED(ChildFolders(Home, children)) || children.empty())
        return 1;
    ComPtr<IACList2> candidates;
    ComPtr<IEnumString> strings;
    bool foundCandidate = false;
    if (SUCCEEDED(
            CoCreateInstance(CLSID_ACListISF, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&candidates))) &&
        SUCCEEDED(candidates->SetOptions(ACLO_CURRENTDIR | ACLO_MYCOMPUTER | ACLO_FILESYSDIRS)) &&
        SUCCEEDED(candidates->Expand((dir.wstring() + L"\\").c_str())) &&
        SUCCEEDED(candidates.As(&strings))) {
        LPOLESTR value = nullptr;
        while (strings->Next(1, &value, nullptr) == S_OK) {
            foundCandidate |= std::wstring(value).find(L"candidate-folder") != std::wstring::npos;
            CoTaskMemFree(value);
        }
    }
    if (!foundCandidate) {
        std::cerr << "system path completion candidate missing\n";
        return 1;
    }
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
        return 1;
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&cc);
    bool failed = false;
    for (int round = 0; round < 2 && !failed; ++round) {
        App app;
        CheckState check{&app, original, background, cached};
        state = &check;
        UINT_PTR timer = SetTimer(nullptr, 0, 100, Check);
        app.Run(GetModuleHandleW(nullptr), SW_HIDE, L"");
        KillTimer(nullptr, timer);
        state = nullptr;
        failed = check.failed || check.stage != 6;
    }
    Gdiplus::GdiplusShutdown(token);
    OleUninitialize();
    return failed ? 1 : 0;
}
