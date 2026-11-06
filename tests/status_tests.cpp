#include "app.h"
#include "shell_view.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace zen;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)
namespace {
class CountingShell final : public IShellView {
    LONG refs = 1;

  public:
    int queries = 0;
    HRESULT queryResult = E_NOINTERFACE;
    std::function<void()> callback;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IOleWindow || iid == IID_IShellView) {
            *out = static_cast<IShellView *>(this);
            AddRef();
            return S_OK;
        }
        if (iid == IID_IFolderView2) {
            ++queries;
            if (callback) {
                auto invoke = std::move(callback);
                callback = {};
                invoke();
            }
            return queryResult;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto count = InterlockedDecrement(&refs);
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND *out) override {
        *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE UIActivate(UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Refresh() override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateViewWindow(IShellView *, LPCFOLDERSETTINGS, IShellBrowser *, RECT *,
                                               HWND *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DestroyViewWindow() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentInfo(LPFOLDERSETTINGS) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SaveViewState() override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SelectItem(PCUITEMID_CHILD, SVSIF) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetItemObject(UINT, REFIID, void **out) override {
        *out = nullptr;
        return E_NOTIMPL;
    }
};
struct IsolatedRoot {
    std::wstring previous;
    IsolatedRoot() {
        wchar_t value[32768]{};
        DWORD length = GetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", value, 32768);
        if (length && length < 32768)
            previous = value;
        auto root = DataDir() / L"status-pipeline";
        CHECK(SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", root.c_str()));
    }
    ~IsolatedRoot() {
        SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", previous.empty() ? nullptr : previous.c_str());
    }
};
LRESULT CALLBACK TestWindow(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto &app = *reinterpret_cast<App *>(data);
    if (message == WM_ZEN_ACTION || message == WM_ZEN_STATUS_FILES)
        return app.Message(message, w, l);
    return DefSubclassProc(window, message, w, l);
}
struct Fixture {
    App app;
    Fixture() {
        app.instance = GetModuleHandleW(nullptr);
        app.window = CreateWindowW(L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr, nullptr,
                                   app.instance, nullptr);
        CHECK(app.window);
        CHECK(SetWindowSubclass(app.window, TestWindow, 1, reinterpret_cast<DWORD_PTR>(&app)));
        app.status = CreateWindowW(STATUSCLASSNAMEW, L"", WS_CHILD, 0, 0, 800, 20, app.window, nullptr,
                                   app.instance, nullptr);
        CHECK(app.status);
    }
    ~Fixture() {
        app.closing = true;
        app.ClearStatusWatch();
        app.views.clear();
        DestroyWindow(app.window);
        app.window = app.status = nullptr;
    }
};
void Drain(App &app) {
    MSG message{};
    int count = 0;
    while (PeekMessageW(&message, app.window, WM_ZEN_ACTION, WM_ZEN_STATUS_FILES, PM_REMOVE)) {
        CHECK(++count < 100);
        DispatchMessageW(&message);
    }
}
ComPtr<CountingShell> AddView(App &app, Id id) {
    ComPtr<CountingShell> shell;
    shell.Attach(new CountingShell);
    auto view = std::make_unique<ShellView>(app, id);
    view->shell = shell;
    view->host =
        CreateWindowW(L"STATIC", L"", WS_CHILD, 0, 0, 10, 10, app.window, nullptr, app.instance, nullptr);
    CHECK(view->host);
    app.views[id] = std::move(view);
    return shell;
}
} // namespace
bool CheckStatusUpdates() {
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
        CHECK(InitCommonControlsEx(&controls));
        IsolatedRoot root;
        Fixture fixture;
        auto &app = fixture.app;
        const auto folder = DataDir() / L"observed";
        std::filesystem::create_directories(folder);
        auto item = ParseItem(folder.wstring());
        CHECK(item);
        const auto key = app.RememberLocation(item.Get());
        const Id first = app.model.panes[0].selected;
        app.model.CommitNavigation(first, key, {key});
        auto shell = AddView(app, first);
        app.visibleTabs[0] = first;
        app.Layout();
        Drain(app);
        CHECK(app.statusWatch && shell->queries > 0);
        const auto watch = app.statusWatch;
        const int initialQueries = shell->queries;
        // 反复布局不再访问 Shell 统计，也不重复创建目录监听。
        for (int i = 0; i < 64; ++i)
            app.Layout();
        CHECK(shell->queries == initialQueries && app.statusWatch == watch);
        Drain(app); // 初次订阅可能收到系统合并的目录通知，不计入布局本身的外呼。
        const int beforeSelection = shell->queries;
        for (int i = 0; i < 64; ++i)
            app.QueueStatusUpdate();
        Drain(app);
        CHECK(shell->queries == beforeSelection + 1);

        // 未改变选择时，实际 Shell 文件通知仍能使缓存失效。
        const auto file = folder / L"changed.txt";
        const int beforeFile = shell->queries;
        std::ofstream(file) << "new bytes";
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, file.c_str(), nullptr);
        const auto deadline = GetTickCount64() + 3000;
        while (shell->queries == beforeFile && GetTickCount64() < deadline) {
            Drain(app);
            Sleep(10);
        }
        CHECK(shell->queries > beforeFile);
        Drain(app);

        SendMessageW(app.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"保留旧文本"));
        shell->callback = [&] {
            CHECK(app.updatingStatus);
            app.QueueStatusUpdate();
            app.UpdateStatus(); // 嵌套统计被拒绝，外层旧结果也不能提交。
        };
        app.UpdateStatus();
        CHECK(WindowText(app.status) == L"保留旧文本" && app.statusUpdatePending);
        Drain(app);
        CHECK(!app.statusUpdatePending && WindowText(app.status) != L"保留旧文本");

        // 模型变更不再采集干净视图；失败采集保留代次并按退避重试。
        app.Save();
        CHECK(!app.views.at(first)->NeedsCapture());
        int before = shell->queries;
        app.Changed();
        app.Save(false);
        CHECK(shell->queries == before && !app.dirty);
        shell->queryResult = E_FAIL;
        app.views.at(first)->MarkSettingsChanged();
        app.Changed();
        app.Save(false);
        CHECK(shell->queries == before + 1 && app.dirty && app.captureFailures == 1 &&
              app.saveRetryAt > GetTickCount64());
        app.Save(false);
        CHECK(shell->queries == before + 1);
        shell->queryResult = E_NOINTERFACE;
        app.Save();
        CHECK(!app.dirty && !app.captureFailures && !app.saveRetryAt);

        Id second = app.model.Add(0, key, 0, -1, false);
        auto other = AddView(app, second);
        app.Save();
        before = shell->queries;
        app.Activate(0, second);
        CHECK(shell->queries == before + 1); // 离开时强制补采集，覆盖未收到通知的设置。
        Drain(app);
        const int otherBefore = other->queries;
        before = shell->queries;
        app.Save();
        CHECK(shell->queries == before + 1 && other->queries == otherBefore + 1);

        // 离开采样中再次切换，后续必须采集真正离开的标签，而非过期的中间选择。
        const Id third = app.model.Add(0, key, 0, -1, false);
        const Id fourth = app.model.Add(0, key, 0, -1, false);
        auto thirdShell = AddView(app, third);
        AddView(app, fourth);
        other->callback = [&] { app.Activate(0, third); };
        app.Activate(0, fourth);
        CHECK(app.model.panes[0].selected == third && app.visibleTabs[0] == third);
        before = thirdShell->queries;
        app.Activate(0, second);
        CHECK(thirdShell->queries == before + 1);
        Drain(app);

        // 虚拟提供程序即使允许注册，也不能假定它提供完整的文件变更通知。
        const LocationKey virtualKey = L"zen-shell:status-virtual";
        auto virtualRecord = *app.model.locations.Find(key);
        virtualRecord.id.clear();
        virtualRecord.parsingName.clear();
        app.model.locations.Set(virtualKey, virtualRecord);
        app.model.Find(second)->path = virtualKey;
        app.Layout();
        Drain(app);
        CHECK(app.statusWatch);
        before = other->queries;
        app.Layout();
        Drain(app);
        CHECK(other->queries == before + 1);
        app.model.Find(second)->path = key;

        // 没有有效目录通知时，保留原来布局触发的刷新机会。
        auto record = *app.model.locations.Find(key);
        record.pidl.clear();
        app.model.locations.Set(key, record);
        app.Layout();
        Drain(app);
        CHECK(!app.statusWatch);
        before = other->queries;
        app.Layout();
        Drain(app);
        CHECK(other->queries == before + 1);
        return true;
    } catch (const std::exception &error) {
        std::cerr << "status pipeline: " << error.what() << '\n';
        return false;
    }
}
