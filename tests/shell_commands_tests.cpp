#include "app.h"
#include "shell_view.h"
#include "shell_menu.h"
#include "operations.h"
#include "tabs.h"
#include <gdiplus.h>
#include <iostream>
#include <fstream>
#include <wrl/implements.h>
using namespace zen;
static App *app;
static std::filesystem::path base;
static bool busy = false, failed = false, legacy = false;
// 手工诊断入口，不加入 CTest；执行已安装的 VS Code 系统菜单。
static bool probeCode = false, probeFolder = false;
static ComPtr<IDataObject> originalClipboard;
static int stage = 0;
static Id openedFrom = 0;
static int backHistory = -1;
static std::filesystem::path feedbackResult;
static TaskItems feedbackItems;
static bool awaitingRename = false;
static ULONGLONG since;
static UINT FindNew(ShellMenu &menu, HMENU h, bool folder) {
    for (int i = 0; i < GetMenuItemCount(h); ++i) {
        wchar_t label[256]{};
        GetMenuStringW(h, i, label, 256, MF_BYPOSITION);
        UINT id = GetMenuItemID(h, i);
        auto verb = menu.Verb(id);
        std::wcout << L"menu " << id << L" " << label << L" verb=" << verb << L"\n";
        if ((folder && !_wcsicmp(verb.c_str(), L"NewFolder")) ||
            (!folder && (wcsstr(label, L"文本文档") || wcsstr(label, L"Text Document"))))
            return id;
        if (HMENU sub = GetSubMenu(h, i)) {
            if (menu.menu3) {
                LRESULT result;
                menu.menu3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)sub, MAKELPARAM(i, FALSE), &result);
            } else if (menu.menu2)
                menu.menu2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)sub, MAKELPARAM(i, FALSE));
            if (UINT found = FindNew(menu, sub, folder))
                return found;
        }
    }
    return 0;
}
static void Fail(const char *text) {
    std::cerr << text << "\n";
    failed = true;
    PostMessageW(app->window, WM_CLOSE, 0, 0);
}
class DirectoryCheck final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IContextMenu> {
    std::wstring expected;

  public:
    explicit DirectoryCheck(std::wstring path) : expected(std::move(path)) {}
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT *, LPSTR, UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO *raw) override {
        if (!raw || raw->cbSize != sizeof(CMINVOKECOMMANDINFOEX) || !(raw->fMask & CMIC_MASK_UNICODE))
            return E_INVALIDARG;
        auto *info = reinterpret_cast<CMINVOKECOMMANDINFOEX *>(raw);
        if (!info->lpDirectoryW || expected != info->lpDirectoryW || !info->lpDirectory)
            return E_FAIL;
        wchar_t ansi[32768]{};
        MultiByteToWideChar(CP_ACP, 0, info->lpDirectory, -1, ansi, 32768);
        return expected == ansi ? S_OK : E_FAIL;
    }
};
static void CheckMenuDirectory(ShellMenu &menu, const std::wstring &path) {
    auto original = menu.context;
    menu.context = Microsoft::WRL::Make<DirectoryCheck>(path);
    auto hr = menu.Invoke(1);
    menu.context = original;
    if (FAILED(hr))
        Fail("Shell menu did not pass its view directory in both encodings");
}
static void CheckModifiedClipboardKeys(ShellView &view) {
    BYTE original[256]{};
    GetKeyboardState(original);
    // 暂时移除 Shell 回退接收者，只验证应用是否误接管；不执行系统扩展快捷键。
    auto shell = view.shell;
    view.shell.Reset();
    HWND focus = GetFocus();
    SetFocus(view.host);
    for (int extra : {VK_SHIFT, VK_MENU, VK_LWIN}) {
        BYTE keys[256]{};
        keys[VK_CONTROL] = 0x80;
        if (extra)
            keys[extra] = 0x80;
        SetKeyboardState(keys);
        for (WPARAM key : {'C', 'X', 'V'}) {
            // 普通复制/粘贴已由实际 Shell 场景覆盖；空接收者只检查不该被接管的组合。
            MSG msg{};
            msg.message = extra == VK_MENU ? WM_SYSKEYDOWN : WM_KEYDOWN;
            msg.wParam = key;
            if (app->Key(msg))
                Fail("clipboard modifier routing mismatch");
        }
    }
    SetKeyboardState(original);
    view.shell = shell;
    SetFocus(focus);
}
static void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (busy || !app || !app->window)
        return;
    busy = true;
    auto id = app->model.panes[stage < 4 ? stage / 2 : 0].selected;
    auto it = app->views.find(id);
    if (GetTickCount64() - since > 15000) {
        std::cerr << "timeout stage=" << stage << "\n";
        Fail("command test timeout");
    } else if (it != app->views.end() && it->second->shell && !it->second->navigating) {
        auto &view = *it->second;
        if (probeCode) {
            if (probeFolder &&
                view.SelectPasteResults({(base / L"files" / L"selected folder").wstring()}) != S_OK) {
                busy = false;
                return;
            }
            ShellMenu menu;
            HRESULT hr = menu.Create(view.shell.Get(), !probeFolder, app->window);
            UINT cmd = SUCCEEDED(hr) ? menu.Find(L"VSCode") : 0;
            if (!cmd) {
                Fail("VSCode background verb missing");
            } else {
                hr = menu.Invoke(cmd);
                std::cout << "VSCode probe folder=" << probeFolder << " hr=" << std::hex << (unsigned long)hr
                          << std::endl;
                failed = FAILED(hr);
            }
            stage = 20;
            PostMessageW(app->window, WM_CLOSE, 0, 0);
            busy = false;
            return;
        }
        if (stage < 4 && awaitingRename) {
            HWND editor = nullptr;
            EnumChildWindows(
                view.host,
                [](HWND h, LPARAM value) -> BOOL {
                    wchar_t name[80]{};
                    GetClassNameW(h, name, 80);
                    if (!_wcsicmp(name, L"Edit") && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE)) {
                        *(HWND *)value = h;
                        return FALSE;
                    }
                    return TRUE;
                },
                (LPARAM)&editor);
            if (editor) {
                if (!ShellView::NativeControlMenu(editor, view.host))
                    Fail("name editor context menu intercepted");
                if (stage % 2 == 0)
                    SetWindowTextW(editor, L"renamed-folder");
                SendMessageW(editor, WM_KEYDOWN, stage % 2 == 0 ? VK_RETURN : VK_ESCAPE, 0);
                awaitingRename = false;
                ++stage;
                since = GetTickCount64();
            } else if (GetTickCount64() - since > 3000) {
                Fail("new item did not enter inline rename");
            }
        } else if (stage < 4) {
            ShellMenu menu;
            HRESULT hr = menu.Create(view.shell.Get(), true, app->window);
            if (SUCCEEDED(hr))
                CheckMenuDirectory(menu, app->model.Find(id)->path);
            UINT cmd = SUCCEEDED(hr) ? FindNew(menu, menu.menu, stage % 2 == 0) : 0;
            if (!cmd)
                Fail("new command missing");
            else {
                if (legacy) {
                    DestroyMenu(menu.menu);
                    menu.menu = nullptr;
                    CMINVOKECOMMANDINFOEX info{sizeof(info)};
                    info.fMask = CMIC_MASK_UNICODE;
                    info.hwnd = app->window;
                    info.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                    info.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                    info.nShow = SW_SHOWNORMAL;
                    hr = menu.context->InvokeCommand((CMINVOKECOMMANDINFO *)&info);
                } else
                    hr = menu.Invoke(cmd);
                std::cout << "invoke result " << std::hex << hr << std::dec << "\n";
                if (FAILED(hr))
                    Fail("new command failed");
            }
            awaitingRename = true;
            since = GetTickCount64();
        } else if (stage == 4) {
            bool folder = false;
            int texts = 0;
            for (auto &entry : std::filesystem::directory_iterator(base / L"files")) {
                folder |= entry.is_directory();
                texts += entry.path().extension() == L".txt";
            }
            if (folder && texts >= 2 && std::filesystem::exists(base / L"files" / L"renamed-folder") &&
                std::filesystem::exists(base / L"destination" / L"renamed-folder") &&
                std::distance(std::filesystem::directory_iterator(base / L"destination"),
                              std::filesystem::directory_iterator{}) == 2) {
                stage = 5;
                view.Refresh();
                since = GetTickCount64();
            }
        } else if (stage == 5) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            auto path = (base / L"files" / L"clipboard-source.txt").wstring();
            if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) {
                HRESULT hr = view.shell->SelectItem(ILFindLastID(pidl),
                                                    SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
                CoTaskMemFree(pidl);
                if (SUCCEEDED(hr) && !view.Selected().empty()) {
                    // 直接调用系统复制，与资源管理器使用相同的 Shell 剪贴板协议。
                    hr = app->ShellVerb(view, L"copy", false);
                    ComPtr<IDataObject> data;
                    OleGetClipboard(&data);
                    auto paths = DataItems(data.Get());
                    if (hr != S_OK || paths.size() != 1 || paths[0] != path)
                        Fail("native copy clipboard mismatch");
                    else {
                        auto &dest = *app->views.at(app->model.panes[1].selected);
                        app->Paste(dest);
                        stage = 6;
                        since = GetTickCount64();
                    }
                }
            }
        } else if (stage == 6) {
            bool done = app->pendingPastes.empty();
            auto tasks = DataDir() / L"tasks";
            if (std::filesystem::exists(tasks))
                for (auto &entry : std::filesystem::directory_iterator(tasks)) {
                    if (entry.path().extension() != L".result")
                        continue;
                    auto result = ReadTaskResult(entry.path());
                    if (FAILED(result.invoked))
                        Fail("independent paste failed");
                    done = true;
                }
            if (done && std::filesystem::exists(base / L"destination" / L"clipboard-source.txt")) {
                std::ifstream file(base / L"destination" / L"clipboard-source.txt");
                std::string value;
                std::getline(file, value);
                if (value != "clipboard regression")
                    Fail("paste contents mismatch");
                auto &dest = *app->views.at(app->model.panes[1].selected);
                HRESULT hr = app->ShellVerb(dest, L"undo", true);
                std::cout << "undo result " << std::hex << hr << std::dec << "\n";
                if (hr != S_OK)
                    Fail("system undo unavailable or failed");
                stage = 7;
                since = GetTickCount64();
            }
        } else if (stage == 7) {
            if (!std::filesystem::exists(base / L"destination" / L"clipboard-source.txt")) {
                // 本程序复制后，使用原生 Shell 粘贴反向验证互通。
                app->CopySelection(view, false);
                auto &dest = *app->views.at(app->model.panes[1].selected);
                HRESULT hr = app->ShellVerb(dest, L"paste", true);
                if (hr != S_OK)
                    Fail("native paste failed");
                stage = 8;
                since = GetTickCount64();
            }
        } else if (stage == 8 && std::filesystem::exists(base / L"destination" / L"clipboard-source.txt")) {
            // 失败/取消的移动不能清除剪贴板，成功结果也不能清除后来复制的内容。
            for (int scenario = 0; scenario < 3; ++scenario) {
                auto result = base / (L"paste-result-" + std::to_wstring(scenario));
                Writer writer;
                writer.Pod<HRESULT>(scenario == 0 ? E_FAIL : S_OK);
                writer.Pod<bool>(scenario == 2);
                writer.Pod<DWORD>(DROPEFFECT_MOVE);
                WriteEnvelope(result, TaskMagic, writer.bytes, false);
                DWORD sequence = GetClipboardSequenceNumber();
                app->pendingPastes.push_back({result, scenario == 2 ? sequence - 1 : sequence});
                app->PollPastes();
                if (GetClipboardSequenceNumber() != sequence)
                    Fail("paste result cleared preserved clipboard");
            }
            // 成功并且仍是同一份剪贴板时才清空。
            auto result = base / L"paste-success";
            Writer writer;
            writer.Pod<HRESULT>(S_OK);
            writer.Pod<bool>(true);
            writer.Pod<DWORD>(DROPEFFECT_MOVE);
            WriteEnvelope(result, TaskMagic, writer.bytes, false);
            app->pendingPastes.push_back({result, GetClipboardSequenceNumber()});
            app->PollPastes();
            ComPtr<IDataObject> remaining;
            OleGetClipboard(&remaining);
            if (!DataItems(remaining.Get()).empty())
                Fail("successful move did not clear clipboard");
            // 操作进程退出却没有结果时必须停止等待；不能清空后来复制的剪贴板。
            App::PendingPaste exited{base / L"worker-exited-without-result", GetClipboardSequenceNumber()};
            exited.process = std::shared_ptr<void>(CreateEventW(nullptr, TRUE, TRUE, nullptr),
                                                   [](void *handle) { CloseHandle(handle); });
            if (!exited.process)
                Fail("exit observation handle unavailable");
            else {
                app->pendingPastes.push_back(std::move(exited));
                app->PollPastes();
                if (!app->pendingPastes.empty())
                    Fail("exited worker remained pending");
            }
            app->Navigate(id, (base / L"empty-after-delete").wstring());
            stage = 9;
            since = GetTickCount64();
        } else if (stage == 9) {
            const auto last = (base / L"empty-after-delete" / L"last.txt").wstring();
            if (view.SelectPasteResults({last}) != S_OK) {
                busy = false;
                return;
            }
            std::vector<std::wstring> paths;
            if (view.DeletePaths(paths) != S_OK || paths != std::vector<std::wstring>{last})
                Fail("delete snapshot did not preserve selected file path");
            std::filesystem::remove(base / L"empty-after-delete" / L"last.txt");
            view.Refresh();
            stage = 10;
            since = GetTickCount64();
        } else if (stage == 10) {
            ComPtr<IFolderView2> folder;
            int count = -1;
            if (SUCCEEDED(view.shell.As(&folder)) && SUCCEEDED(folder->ItemCount(SVGIO_ALLVIEW, &count)) &&
                count == 0) {
                auto &right = *app->views.at(app->model.panes[1].selected);
                PIDLIST_ABSOLUTE source = nullptr;
                auto sourcePath = (base / L"destination" / L"clipboard-source.txt").wstring();
                if (FAILED(SHParseDisplayName(sourcePath.c_str(), nullptr, &source, 0, nullptr))) {
                    Fail("focus paste source missing");
                    busy = false;
                    return;
                }
                HRESULT selected = right.shell->SelectItem(ILFindLastID(source),
                                                           SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
                CoTaskMemFree(source);
                if (FAILED(selected) || right.Selected().empty()) {
                    right.Refresh();
                    busy = false;
                    return;
                }
                app->CopySelection(right, false);
                app->Activate(1, 0, true);
                // 在空视图中新建无焦点的提示子窗口，覆盖尚未被定时 Hook 的窗口。
                HWND empty = CreateWindowW(L"STATIC", L"空目录", WS_CHILD | WS_VISIBLE, 0, 0, 80, 24,
                                           view.host, nullptr, app->instance, nullptr);
                MSG click{};
                click.hwnd = empty;
                click.message = WM_LBUTTONDOWN;
                app->ViewMouse(click);
                HWND focus = GetFocus();
                if (app->model.active != 0 || (focus != view.host && !IsChild(view.host, focus)))
                    Fail("empty folder click did not activate and focus left pane");
                std::vector<std::wstring> paths{L"stale path"};
                if (view.DeletePaths(paths) != S_FALSE || !paths.empty())
                    Fail("empty delete selection was not a no-op");
                const auto tasks = app->retiredTasks.size();
                MSG deletion{};
                deletion.message = WM_KEYDOWN;
                deletion.wParam = VK_DELETE;
                if (!app->Key(deletion))
                    Fail("empty delete key was not consumed");
                deletion.lParam = 1LL << 30; // 长按产生的重复按键同样不应报错或创建任务。
                if (!app->Key(deletion) || app->retiredTasks.size() != tasks)
                    Fail("repeated empty delete created a task");
                DestroyWindow(empty);
                // 真实焦点可纠正过时的活动栏状态，不能把文件快捷键投给另一栏。
                app->model.active = 1;
                MSG key{};
                key.message = WM_KEYDOWN;
                key.wParam = VK_F5;
                app->Key(key);
                if (app->model.active != 0)
                    Fail("keyboard routing used stale active pane");
                app->Paste(*app->views.at(app->model.panes[app->model.active].selected));
                stage = 11;
                since = GetTickCount64();
            }
        } else if (stage == 11 &&
                   std::filesystem::exists(base / L"empty-after-delete" / L"clipboard-source.txt")) {
            std::ifstream copied(base / L"empty-after-delete" / L"clipboard-source.txt");
            std::string content;
            std::getline(copied, content);
            if (content != "clipboard regression")
                Fail("empty pane paste contents mismatch");
            stage = 12;
            since = GetTickCount64();
        } else if (stage == 12) {
            // 在真实 Shell 中验证菜单重命名，而非只验证 F2 或新建后的名称编辑。
            auto path = base / L"empty-after-delete" / L"clipboard-source.txt";
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) {
                view.shell->SelectItem(ILFindLastID(pidl), SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
                CoTaskMemFree(pidl);
                ShellMenu menu;
                HRESULT hr = menu.Create(view.shell.Get(), false, app->window);
                if (SUCCEEDED(hr))
                    CheckMenuDirectory(menu, app->model.Find(id)->path);
                UINT command = SUCCEEDED(hr) ? menu.Find(L"rename") : 0;
                if (!command || FAILED(menu.Invoke(command)))
                    Fail("native rename menu missing or failed");
                stage = 13;
                since = GetTickCount64();
            }
        } else if (stage == 13) {
            HWND editor = nullptr;
            EnumChildWindows(
                view.host,
                [](HWND h, LPARAM value) -> BOOL {
                    wchar_t name[80]{};
                    GetClassNameW(h, name, 80);
                    if (!_wcsicmp(name, L"Edit") && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE)) {
                        *(HWND *)value = h;
                        return FALSE;
                    }
                    return TRUE;
                },
                (LPARAM)&editor);
            if (editor) {
                // Esc 保持原名，验证右键菜单确实启动了原生编辑。
                SendMessageW(editor, WM_KEYDOWN, VK_ESCAPE, 0);
                HWND header = CreateWindowW(WC_HEADERW, L"", WS_CHILD, 0, 0, 1, 1, view.host, nullptr,
                                            app->instance, nullptr);
                if (!header || !ShellView::NativeControlMenu(header, view.host) ||
                    ShellView::NativeControlMenu(view.host, view.host))
                    Fail("native control context menu routing failed");
                DestroyWindow(header);
                HWND list = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT, 0, 0, 100, 100, view.host,
                                          nullptr, app->instance, nullptr);
                HWND edit = CreateWindowW(L"Edit", L"", WS_CHILD, 0, 0, 10, 10, list, nullptr, app->instance,
                                          nullptr);
                HWND other = CreateWindowW(L"Button", L"", WS_CHILD, 0, 0, 10, 10, list, nullptr,
                                           app->instance, nullptr);
                HWND emptyText = CreateWindowW(L"Static", L"", WS_CHILD, 0, 0, 10, 10, list, nullptr,
                                               app->instance, nullptr);
                if (!list || !edit || !other || !emptyText || !ShellView::FileAreaMenu(list, view.host) ||
                    !ShellView::FileAreaMenu(emptyText, view.host) ||
                    ShellView::FileAreaMenu(edit, view.host) || ShellView::FileAreaMenu(other, view.host) ||
                    ShellView::FileAreaMenu(view.host, view.host) ||
                    ShellView::FileAreaMenu(ListView_GetHeader(list), view.host))
                    Fail("context menu intercepted a non-file control");
                if (list) {
                    LVCOLUMNW column{};
                    column.mask = LVCF_WIDTH;
                    column.cx = 90;
                    ListView_InsertColumn(list, 0, &column);
                    LVITEMW item{};
                    item.mask = LVIF_TEXT;
                    wchar_t label[] = L"folder";
                    item.pszText = label;
                    ListView_InsertItem(list, &item);
                    ListView_SetItemState(list, 0, LVIS_SELECTED, LVIS_SELECTED);
                    RECT row{};
                    if (!ListView_GetItemRect(list, 0, &row, LVIR_BOUNDS))
                        Fail("menu hit-test row unavailable");
                    else {
                        POINT itemPoint{20, (row.top + row.bottom) / 2};
                        POINT blankPoint{20, row.bottom + 20};
                        ClientToScreen(list, &itemPoint);
                        ClientToScreen(list, &blankPoint);
                        if (view.ContextMenuBackground(list, itemPoint, false) !=
                                std::optional<bool>{false} ||
                            view.ContextMenuBackground(list, blankPoint, false) !=
                                std::optional<bool>{true} ||
                            ListView_GetSelectedCount(list) != 1)
                            Fail("background menu confused with retained selection");
                    }
                }
                DestroyWindow(list);
                app->CopySelection(view, false);
                auto target = ParseItem((base / L"destination" / L"renamed-folder").wstring());
                if (!target)
                    Fail("paste child folder unavailable");
                else
                    app->Paste(view, target.Get());
                stage = 14;
                since = GetTickCount64();
            }
        } else if (stage == 14 && std::filesystem::exists(base / L"destination" / L"renamed-folder" /
                                                          L"clipboard-source.txt")) {
            if (std::distance(std::filesystem::directory_iterator(base / L"empty-after-delete"),
                              std::filesystem::directory_iterator{}) != 1)
                Fail("targeted paste incorrectly created a same-directory copy");
            std::ifstream copied(base / L"destination" / L"renamed-folder" / L"clipboard-source.txt");
            std::string content;
            std::getline(copied, content);
            if (content != "clipboard regression")
                Fail("targeted paste contents mismatch");
            CheckModifiedClipboardKeys(view);
            BYTE original[256]{}, clear[256]{};
            GetKeyboardState(original);
            SetKeyboardState(clear);
            auto *tab = app->model.Find(id);
            int history = tab->historyIndex;
            auto previous = tab->history.at(history - 1);
            // 编辑框中的 Backspace 必须交回文本控件，不能改变目录。
            HWND edit = CreateWindowW(L"Edit", L"abc", WS_CHILD, 0, 0, 10, 10, view.host, nullptr,
                                      app->instance, nullptr);
            SetFocus(edit);
            MSG back{};
            back.message = WM_KEYDOWN;
            back.wParam = VK_BACK;
            if (app->Key(back) || tab->historyIndex != history)
                Fail("editing Backspace navigated");
            DestroyWindow(edit);
            SetFocus(view.host);
            app->model.active = 1; // 真实焦点必须纠正活动栏，不能后退右栏。
            auto rightPath = app->model.Find(app->model.panes[1].selected)->path;
            backHistory = history - 1;
            if (!app->Key(back) ||
                (view.navigating ? view.pendingHistoryIndex != backHistory || view.expected != previous
                                 : tab->historyIndex != backHistory || tab->path != previous) ||
                app->model.Find(app->model.panes[1].selected)->path != rightPath)
                Fail("Backspace did not navigate focused pane history");
            SetKeyboardState(original);
            stage = 15;
            since = GetTickCount64();
        } else if (stage == 15) {
            if (app->model.Find(id)->path != (base / L"files").wstring() ||
                app->model.Find(id)->historyIndex != backHistory)
                Fail("Backspace used parent directory instead of history");
            PIDLIST_ABSOLUTE folder = nullptr;
            if (FAILED(SHParseDisplayName((base / L"files" / L"renamed-folder").c_str(), nullptr, &folder, 0,
                                          nullptr))) {
                Fail("source-folder selection unavailable");
            } else {
                view.shell->SelectItem(ILFindLastID(folder),
                                       SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
                CoTaskMemFree(folder);
                openedFrom = id;
                app->OpenSelected(view, true);
                auto *opened = app->model.Find(app->model.panes[0].selected);
                if (!opened || opened->id == id || opened->opener != id)
                    Fail("new tab did not record its source");
            }
            stage = 16;
            since = GetTickCount64();
        } else if (stage == 16) {
            app->CloseTabs(0, {id});
            if (app->model.panes[0].selected != openedFrom || !app->views.contains(openedFrom))
                Fail("closing child tab did not restore source view");
            stage = 17;
            since = GetTickCount64();
        } else if (stage == 17) {
            auto source = (base / L"files" / L"clipboard-source.txt").wstring();
            if (view.SelectPasteResults({source}) == S_OK) {
                app->Activate(0, id, true);
                app->CopySelection(view, false);
                app->Paste(view);
                if (app->pendingPastes.empty())
                    Fail("copy result was not tracked");
                else
                    feedbackResult = app->pendingPastes.back().result;
                stage = 18;
                since = GetTickCount64();
            }
        } else if (stage == 18 && std::filesystem::exists(feedbackResult)) {
            feedbackItems = ReadTaskItems(feedbackResult);
            app->PollPastes();
            if (app->pendingPastes.empty()) {
                auto detail = feedbackItems;
                // 隐藏宿主不冒充前台：直接验证真实副本明细到系统选择接口的链路。
                if (view.SelectPasteResults(detail.created) == S_FALSE) {
                    busy = false;
                    return;
                }
                auto selection = view.Selected();
                if (detail.created.size() != 1 || selection.size() != 1 ||
                    ItemName(selection[0].Get()) != detail.created[0])
                    Fail("same-folder paste did not select actual duplicate");
                stage = 19;
                since = GetTickCount64();
            }
        } else if (stage == 19) {
            auto detail = feedbackItems;
            // 使用独立测试回执，真实任务的回执可能已被正常清理。
            feedbackResult = base / L"stale-feedback.result";
            Writer completedResult;
            completedResult.Pod<HRESULT>(S_OK);
            completedResult.Pod<bool>(false);
            completedResult.Pod<DWORD>(DROPEFFECT_COPY);
            WriteEnvelope(feedbackResult, TaskMagic, completedResult.bytes, false);
            auto source = (base / L"files" / L"clipboard-source.txt").wstring();
            if (view.SelectPasteResults({source, detail.created[0]}) != S_OK || view.Selected().size() != 2)
                Fail("batch result selection failed");
            view.SelectPasteResults({source});
            // 已完成任务遇到用户后续操作、切换或旧版无明细时，不得覆盖现有选择。
            for (int scenario = 0; scenario < 6; ++scenario) {
                App::PendingPaste pending{feedbackResult, GetClipboardSequenceNumber()};
                pending.move = false;
                pending.tab = id;
                pending.pane = 0;
                pending.generation = app->pasteGeneration;
                pending.focus = GetFocus();
                pending.destination = ItemName(view.Current().Get());
                pending.created = detail.created;
                pending.read = true;
                pending.deadline = GetTickCount64() + 5000;
                if (!app->PasteFeedbackCurrent(pending, app->window, GetFocus()) ||
                    app->PasteFeedbackCurrent(pending, nullptr, GetFocus()))
                    Fail("feedback foreground eligibility mismatch");
                if (scenario == 0)
                    --pending.generation;
                if (scenario == 1)
                    pending.focus = app->address[0];
                if (scenario == 2)
                    pending.destination = (base / L"destination").wstring();
                if (scenario == 3)
                    pending.tab = app->model.panes[1].selected;
                if (scenario == 4)
                    pending.deadline = 1;
                if (scenario == 5) {
                    pending.result = base / L"legacy-feedback.result";
                    pending.read = false;
                    Writer legacyResult;
                    legacyResult.Pod<HRESULT>(S_OK);
                    legacyResult.Pod<bool>(false);
                    legacyResult.Pod<DWORD>(DROPEFFECT_COPY);
                    WriteEnvelope(pending.result, TaskMagic, legacyResult.bytes, false);
                }
                if (scenario < 5 && app->PasteFeedbackCurrent(pending, app->window, GetFocus()))
                    Fail("changed context remained eligible for feedback");
                app->pendingPastes.push_back(std::move(pending));
                app->PollPastes();
                auto selection = view.Selected();
                if (!app->pendingPastes.empty() || selection.size() != 1 ||
                    ItemName(selection[0].Get()) != source)
                    Fail("stale or missing paste feedback changed selection");
            }
            stage = 20;
            PostMessageW(app->window, WM_CLOSE, 0, 0);
        }
    }
    busy = false;
}
int wmain(int argc, wchar_t **argv) {
    std::cout << std::unitbuf;
    if (FAILED(OleInitialize(nullptr)))
        return 1;
    if (argc == 3 && std::wstring(argv[1]) == L"--task") {
        int result = RunFileTask(GetModuleHandleW(nullptr), argv[2]);
        OleUninitialize();
        return result;
    }
    legacy = argc == 2 && std::wstring(argv[1]) == L"--legacy-lifetime";
    probeCode = argc == 2 && (std::wstring(argv[1]) == L"--probe-vscode" ||
                              std::wstring(argv[1]) == L"--probe-vscode-folder");
    probeFolder = argc == 2 && std::wstring(argv[1]) == L"--probe-vscode-folder";
    // 仅手工诊断进程移除开发终端的 Node 模式，避免 Code.exe 被当作脚本解释器。
    if (probeCode)
        SetEnvironmentVariableW(L"ELECTRON_RUN_AS_NODE", nullptr);
    OleGetClipboard(&originalClipboard);
    base = std::filesystem::current_path() / L".test-data" /
           (L"commands 中文空格-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(base / L"files");
    if (probeCode)
        std::filesystem::create_directories(base / L"files" / L"selected folder");
    SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", base.c_str());
    std::filesystem::create_directories(base / L"destination");
    std::filesystem::create_directories(base / L"empty-after-delete");
    std::ofstream(base / L"empty-after-delete" / L"last.txt") << "last file";
    std::ofstream(base / L"files" / L"clipboard-source.txt") << "clipboard regression";
    Model model;
    model.Navigate(model.panes[1].selected, (base / L"destination").wstring());
    model.Navigate(model.panes[0].selected, (base / L"files").wstring());
    std::filesystem::create_directories(base / L"config");
    if (!SaveModel(model, base / L"config" / L"session.dat"))
        return 1;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&cc);
    {
        App instance;
        app = &instance;
        since = GetTickCount64();
        UINT_PTR timer = SetTimer(nullptr, 0, 250, Tick);
        instance.Run(GetModuleHandleW(nullptr), SW_HIDE, L"");
        KillTimer(nullptr, timer);
        app = nullptr;
    }
    Gdiplus::GdiplusShutdown(token);
    if (originalClipboard) {
        OleSetClipboard(originalClipboard.Get());
        OleFlushClipboard();
    } else
        OleSetClipboard(nullptr);
    originalClipboard.Reset();
    OleUninitialize();
    return failed || stage != 20 ? 1 : 0;
}
