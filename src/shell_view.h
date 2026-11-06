#pragma once
#include "app.h"
namespace zen {
class ShellView : public IExplorerBrowserEvents,
                  public IServiceProvider,
                  public ICommDlgBrowser3,
                  public IDropTarget {
    LONG refs = 1;
    DWORD cookie = 0;
    bool advised = false;
    void Trace(const std::wstring &phase, HRESULT hr = S_OK);
    std::vector<HWND> hooks;
    std::vector<HWND> drops;
    ComPtr<IDataObject> dragging;
    ComPtr<IDropTarget> dragTarget;
    ComPtr<IShellItem> dragDestination;
    DWORD dragButtons = 0;
    std::wstring dragHitName;
    ComPtr<IShellItem> dragHitItem;
    bool destroyed = false;
    bool initialized = false, creating = false;
    bool browserSized = false;
    bool capturing = false;
    // 全部视图在 UI 线程创建；防止重入销毁后，同 ID 的新对象复用原地址。
    inline static uint64_t nextLifetimeId = 0;
    const uint64_t lifetimeId = ++nextLifetimeId;
    uint64_t settingsRevision = 1, capturedRevision = 0;
    RECT browserBounds{};
    NavigationState navigation;
    NavigationState previousNavigation;
    LocationKey navigationPath;
    void UpdateNavigation(IShellItem *item);
    void NavigationFailed(HRESULT result);

  public:
    App &app;
    Id id;
    HWND host{};
    ComPtr<IExplorerBrowser> browser;
    ComPtr<IShellView> shell;
    const NavigationState &Navigation() const {
        return navigation;
    }
    void SetFailure(HRESULT result) {
        navigation.Fail(result);
    }
    ShellView(App &, Id);
    ~ShellView();
    HRESULT Create(HWND parent, RECT bounds);
    bool Initialized() const {
        return initialized;
    }
    HRESULT Browse(const NavigationRequest &request, bool fallback = false);
    void Retry();
    void Destroy();
    bool NeedsCapture() const {
        return settingsRevision != capturedRevision;
    }
    void MarkSettingsChanged() {
        ++settingsRevision;
    }
    // 强制采样；自动保存可先检查 NeedsCapture。失败或重入变更时保留待采集标记。
    bool Capture();
    void Apply();
    HRESULT ApplyViewMode();
    void Hook();
    void Resize(RECT);
    void Show(bool);
    void Refresh();
    // 仅当所有结果已出现在本视图中才批量选中；不抢键盘焦点。S_FALSE 表示稍后重试。
    HRESULT SelectPasteResults(const std::vector<std::wstring> &paths);
    void Up();
    bool CanUp() const;
    std::vector<std::pair<std::wstring, LocationKey>> AddressParts() const;
    std::wstring AddressEditText() const;
    std::vector<ComPtr<IShellItem>> Selected();
    ComPtr<IDataObject> SelectedData();
    // 删除只快照文件系统路径；S_FALSE 表示没有选中项，失败时不返回部分项目。
    HRESULT DeletePaths(std::vector<std::wstring> &paths);
    ComPtr<IShellItem> Current();
    ComPtr<IShellItem> DropItem(POINTL point);
    static LRESULT CALLBACK Subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static bool NativeControlMenu(HWND source, HWND host);
    static bool FileAreaMenu(HWND source, HWND host);
    // true 为目录背景，false 为项目；无法可靠识别时保留原生菜单。
    std::optional<bool> ContextMenuBackground(HWND input, POINT point, bool keyboard);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return InterlockedDecrement(&refs);
    }
    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override;
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView *) override;
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE) override;
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override;
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID, REFIID, void **) override;
    HRESULT STDMETHODCALLTYPE OnDefaultCommand(IShellView *) override;
    HRESULT STDMETHODCALLTYPE OnStateChange(IShellView *, ULONG) override;
    HRESULT STDMETHODCALLTYPE IncludeObject(IShellView *, PCUITEMID_CHILD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Notify(IShellView *, DWORD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDefaultMenuText(IShellView *, LPWSTR, int) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetViewFlags(DWORD *f) override {
        *f = CDB2GVF_NOINCLUDEITEM;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnColumnClicked(IShellView *, int) override;
    HRESULT STDMETHODCALLTYPE GetCurrentFilter(LPWSTR p, int n) override {
        if (n)
            p[0] = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPreViewCreated(IShellView *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL, DWORD *) override;
};
} // namespace zen
