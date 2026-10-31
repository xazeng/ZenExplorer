#include "app.h"
#include "shell_view.h"
#include "tabs.h"
#include <gdiplus.h>
#include <iostream>
using namespace zen;
struct CheckState {
    App *app;
    Id original, background, cached;
    int stage = 0;
    ULONGLONG since = GetTickCount64();
    bool failed = false, busy = false;
};
static CheckState *state;
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
    if (!state || state->busy || !state->app->window)
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
