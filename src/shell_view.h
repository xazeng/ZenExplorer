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
    bool browserSized = false;
    RECT browserBounds{};
    NavigationState navigation;
    LocationKey navigationPath, parentPath;
    std::wstring addressEditText;
    std::vector<std::pair<std::wstring, LocationKey>> breadcrumbs;
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
    HRESULT Browse(const NavigationRequest &request);
    void Retry();
    void Destroy();
    void Capture();
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
    HRESULT STDMETHODCALLTYPE OnColumnClicked(IShellView *, int) override {
        return S_OK;
    }
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
