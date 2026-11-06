#include "app.h"
#include "shell_view.h"
#include <iostream>

using namespace zen;
namespace {
// 保留真实目录接口，只在查询入口计数并注入重入，避免依赖目录枚举时序。
class PasteQueryShell final : public IShellView {
    LONG refs = 1;

  public:
    ComPtr<IShellView> actual;
    int queries = 0;
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
            return actual->QueryInterface(iid, out);
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&refs);
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
        return E_NOTIMPL;
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
} // namespace

bool CheckPasteFeedbackFiltering(App &app) {
    bool ok = true;
    auto expect = [&](bool condition, const char *message) {
        if (!condition) {
            std::cerr << message << "\n";
            ok = false;
        }
    };
    const int pane = app.model.active;
    const Id original = app.model.panes[pane].selected;
    auto source = app.views.find(original);
    if (source == app.views.end() || !source->second->shell) {
        std::cerr << "paste feedback regression requires a live Shell view\n";
        return false;
    }
    const auto path = app.model.Find(original)->path;
    const auto generation = app.pasteGeneration;
    PasteFeedback pending;
    pending.pane = pane;
    pending.focus = GetFocus();
    pending.generation = generation;
    pending.deadline = GetTickCount64() + 5000;
    pending.destination = ItemName(source->second->Current().Get());
    if (pending.destination.empty()) {
        std::cerr << "paste feedback regression requires a named directory\n";
        return false;
    }
    ComPtr<PasteQueryShell> probe;
    probe.Attach(new PasteQueryShell);
    probe->actual = source->second->shell;
    pending.tab = app.model.Add(pane, path, 0, -1, false);
    auto view = std::make_unique<ShellView>(app, pending.tab);
    view->shell = probe;
    app.views[pending.tab] = std::move(view);
    app.model.panes[pane].selected = pending.tab;

    auto rejectedWithoutQuery = [&](const PasteFeedback &candidate, HWND foreground, HWND focus) {
        probe->queries = 0;
        expect(!app.tasks.PasteCurrent(candidate, foreground, focus) && probe->queries == 0,
               "stale paste feedback queried the Shell");
    };
    rejectedWithoutQuery(pending, nullptr, pending.focus);
    rejectedWithoutQuery(pending, app.window, reinterpret_cast<HWND>(1));
    auto invalid = pending;
    invalid.deadline = 0;
    rejectedWithoutQuery(invalid, app.window, pending.focus);
    invalid = pending;
    ++invalid.generation;
    rejectedWithoutQuery(invalid, app.window, pending.focus);
    invalid = pending;
    invalid.pane = -1;
    rejectedWithoutQuery(invalid, app.window, pending.focus);
    app.model.active = 1 - pane;
    rejectedWithoutQuery(pending, app.window, pending.focus);
    app.model.active = pane;
    app.model.panes[pane].selected = original;
    rejectedWithoutQuery(pending, app.window, pending.focus);
    app.model.panes[pane].selected = pending.tab;
    auto detached = app.views.extract(pending.tab);
    rejectedWithoutQuery(pending, app.window, pending.focus);
    app.views.insert(std::move(detached));

    probe->queries = 0;
    expect(app.tasks.PasteCurrent(pending, app.window, pending.focus) && probe->queries == 1,
           "current paste feedback did not query and accept its directory");
    invalid = pending;
    invalid.destination += L"-other";
    expect(!app.tasks.PasteCurrent(invalid, app.window, pending.focus),
           "paste feedback accepted a different destination");

    auto rejectedAfterQuery = [&](std::function<void()> callback) {
        probe->queries = 0;
        probe->callback = std::move(callback);
        expect(!app.tasks.PasteCurrent(pending, app.window, pending.focus) && probe->queries == 1,
               "paste feedback accepted state changed by a Shell query");
    };
    rejectedAfterQuery([&] { ++app.pasteGeneration; });
    app.pasteGeneration = generation;
    rejectedAfterQuery([&] { app.model.Find(pending.tab)->path = L"paste-feedback-changed-path"; });
    app.model.Find(pending.tab)->path = path;
    rejectedAfterQuery([&] { app.views.at(pending.tab)->shell = probe->actual; });
    app.views.at(pending.tab)->shell = probe;
    rejectedAfterQuery([&] { pending.deadline = 0; });
    pending.deadline = GetTickCount64() + 5000;
    // 删除宿主但保留查询接口，确认查询返回后不会访问失效的 ShellView。
    rejectedAfterQuery([&] { app.views.erase(pending.tab); });

    app.views.erase(pending.tab);
    app.model.Remove(pane, {pending.tab}, false);
    app.model.panes[pane].selected = original;
    app.model.active = pane;
    app.pasteGeneration = generation;
    return ok;
}
