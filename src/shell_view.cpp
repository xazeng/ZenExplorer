#include "shell_view.h"
#include "operations.h"
#include <unordered_set>
#include <oleacc.h>
namespace zen {
// 在已知文件视图内部命中，避免系统拖动图像/提示窗口遮住全局 Point 查询。
static HWND ViewWindowAt(HWND host, POINT screen) {
    HWND current = host;
    for (int depth = 0; current && depth < 16; ++depth) {
        POINT local = screen;
        ScreenToClient(current, &local);
        RECT bounds{};
        GetClientRect(current, &bounds);
        if (!PtInRect(&bounds, local))
            return nullptr;
        HWND child = ChildWindowFromPointEx(current, local, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!child || child == current)
            return current;
        current = child;
    }
    return current;
}
static HRESULT ViewAccessibleAt(HWND input, POINT point, IAccessible **result, VARIANT *child) {
    if (!input || !result || !child)
        return E_INVALIDARG;
    *result = nullptr;
    ComPtr<IAccessible> accessible;
    HRESULT hr =
        AccessibleObjectFromWindow(input, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&accessible));
    if (FAILED(hr))
        return hr;
    hr = accessible->accHitTest(point.x, point.y, child);
    if (FAILED(hr))
        return hr;
    if (child->vt == VT_DISPATCH && child->pdispVal) {
        ComPtr<IAccessible> item;
        hr = child->pdispVal->QueryInterface(IID_PPV_ARGS(&item));
        VariantClear(child);
        if (FAILED(hr))
            return hr;
        accessible = item;
        child->vt = VT_I4;
        child->lVal = CHILDID_SELF;
    }
    if (child->vt != VT_I4)
        return E_FAIL;
    *result = accessible.Detach();
    return S_OK;
}
ShellView::ShellView(App &a, Id i) : app(a), id(i) {}
ShellView::~ShellView() {
    Destroy();
}
HRESULT ShellView::QueryInterface(REFIID riid, void **p) {
    if (!p)
        return E_POINTER;
    *p = nullptr;
    if (riid == IID_IUnknown || riid == IID_IExplorerBrowserEvents)
        *p = static_cast<IExplorerBrowserEvents *>(this);
    else if (riid == IID_IServiceProvider)
        *p = static_cast<IServiceProvider *>(this);
    else if (riid == IID_ICommDlgBrowser || riid == IID_ICommDlgBrowser2 || riid == IID_ICommDlgBrowser3)
        *p = static_cast<ICommDlgBrowser3 *>(this);
    else if (riid == IID_IDropTarget)
        *p = static_cast<IDropTarget *>(this);
    if (!*p)
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
HRESULT ShellView::QueryService(REFGUID service, REFIID iid, void **p) {
    if (service == SID_SExplorerBrowserFrame)
        return QueryInterface(iid, p);
    *p = nullptr;
    return E_NOINTERFACE;
}
void ShellView::Trace(const std::wstring &phase, HRESULT hr) {
    RECT parent{}, child{};
    HWND viewWindow = nullptr;
    if (host)
        GetClientRect(host, &parent);
    if (shell && SUCCEEDED(shell->GetWindow(&viewWindow)))
        GetClientRect(viewWindow, &child);
    Log(L"Shell tab=" + std::to_wstring(id) + L" pane=" + std::to_wstring(app.model.PaneOf(id)) + L" " +
        phase + L" host=" + std::to_wstring(parent.right) + L"x" + std::to_wstring(parent.bottom) +
        L" view=" + std::to_wstring(child.right) + L"x" + std::to_wstring(child.bottom) +
        (FAILED(hr) ? L" " + ErrorText(hr) : L""));
}
HRESULT ShellView::Create(HWND parent, RECT bounds) {
    if (!IsWindow(parent) || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return E_INVALIDARG;
    host = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, bounds.left,
                           bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, parent,
                           nullptr, app.instance, nullptr);
    if (!host)
        return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr =
        CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browser));
    if (FAILED(hr))
        return hr;
    ComPtr<IObjectWithSite> site;
    hr = browser.As(&site);
    if (SUCCEEDED(hr))
        hr = site->SetSite(static_cast<IServiceProvider *>(this));
    if (FAILED(hr)) {
        Trace(L"SetSite failed", hr);
        return hr;
    }
    hr = browser->SetOptions(EBO_NOBORDER | EBO_NOPERSISTVIEWSTATE | EBO_NOTRAVELLOG);
    if (FAILED(hr)) {
        Trace(L"SetOptions failed", hr);
        return hr;
    }
    hr = browser->Advise(this, &cookie);
    if (FAILED(hr)) {
        Trace(L"Advise failed", hr);
        return hr;
    }
    advised = true;
    // 必须以实际尺寸初始化。异步创建的 Shell 子视图可能保留最初尺寸，
    // 即使外部容器和 ExplorerBrowser 的目标矩形已经提前调整过。
    RECT inner{0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
    FOLDERSETTINGS settings{FVM_DETAILS, FWF_AUTOARRANGE};
    hr = browser->Initialize(host, &inner, &settings);
    Trace(L"Initialize", hr);
    return hr;
}
void ShellView::Destroy() {
    if (destroyed)
        return;
    destroyed = true;
    ResourceTrace trace(L"销毁 Shell 视图 tab=" + std::to_wstring(id));
    // 撤销注册可能重入窗口消息，先转移列表，避免回调修改正在遍历的容器。
    auto registered = std::move(drops);
    drops.clear();
    for (auto h : registered)
        if (IsWindow(h))
            RevokeDragDrop(h);
    for (auto h : hooks)
        if (IsWindow(h))
            RemoveWindowSubclass(h, Subclass, (UINT_PTR)this);
    hooks.clear();
    DragLeave();
    dragging.Reset();
    shell.Reset();
    if (browser) {
        if (advised)
            browser->Unadvise(cookie);
        advised = false;
        ComPtr<IObjectWithSite> s;
        browser.As(&s);
        if (s)
            s->SetSite(nullptr);
        browser->Destroy();
        browser.Reset();
    }
    if (host)
        DestroyWindow(host);
    host = nullptr;
}
HRESULT ShellView::Browse(const std::wstring &path, bool append) {
    if (!browser || path.empty())
        return E_INVALIDARG;
    // 调用者在修改地址前捕获旧视图；此处不能覆盖已重置的模式。
    Trace(L"Browse start");
    requestedPidl.clear();
    expected = path;
    appendHistory = append;
    navigating = true;
    error = S_OK;
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
    if (SUCCEEDED(hr)) {
        requestedPidl.assign((BYTE *)pidl, (BYTE *)pidl + ILGetSize(pidl));
        hr = browser->BrowseToIDList(pidl, SBSP_ABSOLUTE);
        CoTaskMemFree(pidl);
    }
    if (FAILED(hr)) {
        navigating = false;
        error = hr;
        app.ShowError(id, hr);
    }
    return hr;
}
HRESULT ShellView::OnNavigationPending(PCIDLIST_ABSOLUTE pidl) {
    ++app.pasteGeneration;
    if (!requestedPidl.empty() && !ILIsEqual((PCIDLIST_ABSOLUTE)requestedPidl.data(), pidl))
        return E_ABORT;
    Capture();
    navigating = true;
    return S_OK;
}
HRESULT ShellView::OnViewCreated(IShellView *v) {
    shell = v;
    Trace(L"OnViewCreated");
    Apply();
    PostMessageW(app.window, WM_ZEN_CHANGED, (WPARAM)id, 0);
    return S_OK;
}
HRESULT ShellView::OnNavigationComplete(PCIDLIST_ABSOLUTE pidl) {
    if (destroyed || (!requestedPidl.empty() && !ILIsEqual((PCIDLIST_ABSOLUTE)requestedPidl.data(), pidl)))
        return S_OK;
    requestedPidl.clear();
    ComPtr<IShellItem> item;
    SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
    auto path = ItemName(item.Get());
    if (!path.empty()) {
        app.model.Navigate(id, path, appendHistory);
        app.RefreshMetadata(id, item.Get());
        UpdateNavigation(item.Get());
    }
    navigating = false;
    error = S_OK;
    expected.clear();
    appendHistory = true;
    Trace(L"OnNavigationComplete");
    Apply();
    PostMessageW(app.window, WM_ZEN_CHANGED, (WPARAM)id, 0);
    return S_OK;
}
HRESULT ShellView::OnNavigationFailed(PCIDLIST_ABSOLUTE pidl) {
    if (!requestedPidl.empty() && pidl && !ILIsEqual((PCIDLIST_ABSOLUTE)requestedPidl.data(), pidl))
        return S_OK;
    requestedPidl.clear();
    navigating = false;
    error = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    Trace(L"OnNavigationFailed", error);
    PostMessageW(app.window, WM_ZEN_CHANGED, (WPARAM)id, 1);
    return S_OK;
}
HRESULT ShellView::OnDefaultCommand(IShellView *) {
    auto items = Selected();
    if (items.empty())
        return S_FALSE;
    SFGAOF attr = 0;
    if (items.size() == 1 && SUCCEEDED(items[0]->GetAttributes(SFGAO_FOLDER, &attr)) &&
        (attr & SFGAO_FOLDER)) {
        auto *r = new NavRequest{id, ItemName(items[0].Get()), false};
        if (!PostMessageW(app.window, WM_ZEN_NAV, 0, (LPARAM)r))
            delete r;
        return S_OK;
    }
    return S_FALSE;
}
HRESULT ShellView::OnStateChange(IShellView *, ULONG change) {
    if (change == CDBOSC_SELCHANGE)
        ++app.pasteGeneration;
    if (change == CDBOSC_SETFOCUS) {
        int p = app.model.PaneOf(id);
        if (p >= 0 && app.model.panes[p].selected == id) {
            app.model.active = p;
            PostMessageW(app.window, WM_ZEN_CHANGED, (WPARAM)id, 2);
        }
    } else
        PostMessageW(app.window, WM_ZEN_CHANGED, (WPARAM)id, 3);
    return S_OK;
}
void ShellView::Resize(RECT r) {
    MoveWindow(host, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    RECT inner{0, 0, r.right - r.left, r.bottom - r.top};
    if (browser) {
        HRESULT hr = browser->SetRect(nullptr, inner);
        if (FAILED(hr))
            Trace(L"SetRect failed", hr);
    }
}
void ShellView::Show(bool show) {
    if (host)
        ShowWindow(host, show ? SW_SHOW : SW_HIDE);
}
void ShellView::Refresh() {
    if (shell)
        shell->Refresh();
}
HRESULT ShellView::SelectPasteResults(const std::vector<std::wstring> &paths) {
    if (!shell || paths.empty() || paths.size() > 100000 || navigating)
        return E_INVALIDARG;
    std::unordered_set<std::wstring> wanted(paths.begin(), paths.end());
    if (wanted.size() != paths.size())
        return E_INVALIDARG;
    ComPtr<IFolderView2> folder;
    ComPtr<IShellItemArray> items;
    HRESULT hr = shell.As(&folder);
    if (FAILED(hr))
        return hr;
    hr = folder->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items));
    if (FAILED(hr))
        return S_FALSE;
    DWORD count = 0;
    if (FAILED(items->GetCount(&count)))
        return S_FALSE;
    std::vector<PIDLIST_ABSOLUTE> owned;
    struct ReleasePidls {
        std::vector<PIDLIST_ABSOLUTE> &items;
        ~ReleasePidls() {
            for (auto p : items)
                CoTaskMemFree(p);
        }
    } release{owned};
    // 使用视图实际枚举的项目，不按源文件名猜副本名，也不强制刷新打断当前状态。
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, &item)))
            continue;
        auto name = ItemName(item.Get());
        if (!wanted.contains(name))
            continue;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl)))
            owned.push_back(pidl);
    }
    if (owned.size() != paths.size())
        return S_FALSE;
    std::vector<PCUITEMID_CHILD> children;
    for (auto pidl : owned)
        children.push_back(ILFindLastID(pidl));
    hr = folder->SelectAndPositionItems((UINT)children.size(), children.data(), nullptr,
                                        SVSI_SELECT | SVSI_DESELECTOTHERS);
    if (SUCCEEDED(hr))
        shell->SelectItem(children.front(), SVSI_SELECT | SVSI_ENSUREVISIBLE);
    return hr;
}
ComPtr<IShellItem> ShellView::Current() {
    ComPtr<IShellItem> out;
    ComPtr<IFolderView2> f;
    if (shell && SUCCEEDED(shell.As(&f))) {
        ComPtr<IPersistFolder2> folder;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(f->GetFolder(IID_PPV_ARGS(&folder))) && SUCCEEDED(folder->GetCurFolder(&pidl))) {
            SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&out));
            CoTaskMemFree(pidl);
        }
    }
    return out;
}
void ShellView::UpdateNavigation(IShellItem *item) {
    navigationPath = ItemName(item);
    parentPath.clear();
    breadcrumbs = Breadcrumbs(navigationPath);
    if (!item || navigationPath.empty())
        return;

    // 只在导航完成时查询 Shell；重绘和工具栏更新不解析网络路径。
    ComPtr<IShellItem> parent;
    if (CanNavigateUp(navigationPath) && SUCCEEDED(item->GetParent(&parent))) {
        auto path = ItemName(parent.Get());
        if (!path.empty() && _wcsicmp(path.c_str(), navigationPath.c_str()) != 0)
            parentPath = std::move(path);
    }
    bool filePath =
        navigationPath.starts_with(L"\\\\") || (navigationPath.size() >= 3 && navigationPath[1] == L':');
    std::vector<std::pair<std::wstring, std::wstring>> hierarchy;
    ComPtr<IShellItem> current = item;
    std::unordered_set<std::wstring> seen;
    for (size_t depth = 0; current && depth < 256; ++depth) {
        auto path = ItemName(current.Get());
        if (path.empty() || !seen.insert(path).second)
            break;
        auto label = ItemName(current.Get(), SIGDN_PARENTRELATIVEFORADDRESSBAR);
        if (label.empty())
            label = ItemName(current.Get(), SIGDN_NORMALDISPLAY);
        if (label.empty())
            label = LocationName(path);
        hierarchy.push_back({label, path});
        if (filePath) {
            // 保留既定的此电脑、磁盘或服务器/共享层级，只替换系统显示名。
            for (auto &[name, target] : breadcrumbs)
                if (_wcsicmp(target.c_str(), path.c_str()) == 0)
                    name = label;
        }
        if (!CanNavigateUp(path))
            break;
        ComPtr<IShellItem> next;
        if (FAILED(current->GetParent(&next)))
            break;
        current = std::move(next);
    }
    if (!filePath && !hierarchy.empty()) {
        std::reverse(hierarchy.begin(), hierarchy.end());
        breadcrumbs = std::move(hierarchy);
    }
}
std::vector<std::pair<std::wstring, std::wstring>> ShellView::AddressParts() const {
    auto *tab = app.model.Find(id);
    if (!tab)
        return {};
    return tab->path == navigationPath ? breadcrumbs : Breadcrumbs(tab->path);
}
bool ShellView::CanUp() const {
    auto *tab = app.model.Find(id);
    return tab && !navigating && SUCCEEDED(error) && tab->path == navigationPath &&
           CanNavigateUp(tab->path) && !parentPath.empty();
}
void ShellView::Up() {
    if (!CanUp())
        return;
    auto destination = parentPath;
    app.Navigate(id, destination);
}
std::vector<ComPtr<IShellItem>> ShellView::Selected() {
    std::vector<ComPtr<IShellItem>> out;
    ComPtr<IFolderView2> f;
    ComPtr<IShellItemArray> a;
    if (shell && SUCCEEDED(shell.As(&f)) &&
        SUCCEEDED(f->Items(SVGIO_SELECTION | SVGIO_FLAG_VIEWORDER, IID_PPV_ARGS(&a)))) {
        DWORD n = 0;
        a->GetCount(&n);
        for (DWORD i = 0; i < n; ++i) {
            ComPtr<IShellItem> x;
            if (SUCCEEDED(a->GetItemAt(i, &x)))
                out.push_back(x);
        }
    }
    return out;
}
ComPtr<IDataObject> ShellView::SelectedData() {
    ComPtr<IDataObject> d;
    if (shell)
        shell->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&d));
    return d;
}
void ShellView::Capture() {
    auto *t = app.model.Find(id);
    if (!t || !shell || navigating)
        return;
    ComPtr<IFolderView2> f;
    if (SUCCEEDED(shell.As(&f))) {
        FOLDERVIEWMODE mode;
        if (SUCCEEDED(f->GetViewModeAndIconSize(&mode, &t->iconSize)))
            t->viewMode = mode;
        int n = 0;
        if (SUCCEEDED(f->GetSortColumnCount(&n)) && n > 0 && n < 64) {
            t->sort.resize(n);
            f->GetSortColumns(t->sort.data(), n);
        }
        ComPtr<IColumnManager> cm;
        auto folder = Current();
        SFGAOF folderFlags = 0;
        bool filesystem = folder && SUCCEEDED(folder->GetAttributes(SFGAO_FILESYSTEM, &folderFlags)) &&
                          (folderFlags & SFGAO_FILESYSTEM);
        if (filesystem && SUCCEEDED(shell.As(&cm))) {
            UINT count = 0;
            if (SUCCEEDED(cm->GetColumnCount(CM_ENUM_VISIBLE, &count)) && count < 128) {
                std::vector<PROPERTYKEY> keys(count);
                if (SUCCEEDED(cm->GetColumns(CM_ENUM_VISIBLE, keys.data(), count))) {
                    t->columns.clear();
                    for (auto key : keys) {
                        CM_COLUMNINFO info{sizeof(info)};
                        info.dwMask = CM_MASK_WIDTH;
                        if (SUCCEEDED(cm->GetColumnInfo(key, &info)))
                            t->columns.push_back({key, info.uWidth});
                    }
                }
            }
        }
    }
}
HRESULT ShellView::ApplyViewMode() {
    auto *tab = app.model.Find(id);
    if (!tab || !shell)
        return E_UNEXPECTED;
    ComPtr<IFolderView2> view;
    HRESULT hr = shell.As(&view);
    if (SUCCEEDED(hr))
        hr = view->SetViewModeAndIconSize((FOLDERVIEWMODE)tab->viewMode, tab->iconSize);
    if (FAILED(hr))
        Trace(L"SetViewModeAndIconSize failed", hr);
    return hr;
}
void ShellView::Apply() {
    auto *t = app.model.Find(id);
    if (!t || !shell)
        return;
    ComPtr<IFolderView2> f;
    if (SUCCEEDED(shell.As(&f))) {
        ApplyViewMode();
        if (!t->sort.empty())
            f->SetSortColumns(t->sort.data(), (int)t->sort.size());
        ComPtr<IColumnManager> cm;
        auto folder = Current();
        SFGAOF folderFlags = 0;
        bool filesystem = folder && SUCCEEDED(folder->GetAttributes(SFGAO_FILESYSTEM, &folderFlags)) &&
                          (folderFlags & SFGAO_FILESYSTEM);
        // 没有保存列设置时不干预系统默认列与宽度。
        if (filesystem && !t->columns.empty() && SUCCEEDED(shell.As(&cm))) {
            const auto &cols = t->columns;
            std::vector<PROPERTYKEY> keys;
            for (auto &c : cols)
                keys.push_back(c.key);
            if (!keys.empty() && SUCCEEDED(cm->SetColumns(keys.data(), (UINT)keys.size())))
                for (auto &c : cols) {
                    CM_COLUMNINFO info{sizeof(info)};
                    info.dwMask = CM_MASK_WIDTH;
                    info.uWidth = c.width;
                    cm->SetColumnInfo(c.key, &info);
                }
        }
    }
}
void ShellView::Hook() {
    if (!host)
        return;
    EnumChildWindows(
        host,
        [](HWND h, LPARAM param) -> BOOL {
            auto *s = (ShellView *)param;
            if (std::find(s->hooks.begin(), s->hooks.end(), h) == s->hooks.end()) {
                SetWindowSubclass(h, Subclass, (UINT_PTR)s, (DWORD_PTR)s);
                s->hooks.push_back(h);
                wchar_t name[128];
                GetClassNameW(h, name, 128);
                if (!wcscmp(name, L"SysListView32") || !wcscmp(name, L"DirectUIHWND")) {
                    HRESULT revoke = RevokeDragDrop(h);
                    HRESULT registered = RegisterDragDrop(h, s);
                    if (SUCCEEDED(registered))
                        s->drops.push_back(h);
                    Log(L"文件视图拖放注册 tab=" + std::to_wstring(s->id) + L" class=" + name + L" revoke=" +
                        std::to_wstring((unsigned long)revoke) + L" register=" +
                        std::to_wstring((unsigned long)registered));
                }
            }
            return TRUE;
        },
        (LPARAM)this);
}
bool ShellView::NativeControlMenu(HWND source, HWND host) {
    // WM_CONTEXTMENU 可能由子控件向父窗口转发，沿来源窗口检查，避免再次接管。
    for (HWND h = source; h && h != host && IsChild(host, h); h = GetParent(h)) {
        wchar_t name[128]{};
        GetClassNameW(h, name, 128);
        if (!_wcsicmp(name, L"Edit") || !_wcsicmp(name, WC_HEADERW))
            return true;
    }
    return false;
}
bool ShellView::FileAreaMenu(HWND source, HWND host) {
    if (!source || !host || !IsChild(host, source) || NativeControlMenu(source, host))
        return false;
    // 系统容器与空目录提示也可能接收背景右键；不穿过可交互的未知控件。
    for (HWND current = source; current && current != host; current = GetParent(current)) {
        wchar_t name[128]{};
        GetClassNameW(current, name, 128);
        if (!_wcsicmp(name, WC_LISTVIEWW) || !_wcsicmp(name, L"DirectUIHWND") ||
            !_wcsicmp(name, L"SHELLDLL_DefView") || !_wcsicmp(name, L"DUIViewWndClassName"))
            return true;
        if (_wcsicmp(name, L"Static"))
            return false;
    }
    return false;
}
std::optional<bool> ShellView::ContextMenuBackground(HWND input, POINT point, bool keyboard) {
    if (!FileAreaMenu(input, host))
        return std::nullopt;
    if (keyboard)
        return Selected().empty();
    wchar_t name[128]{};
    GetClassNameW(input, name, 128);
    if (!_wcsicmp(name, WC_LISTVIEWW)) {
        LVHITTESTINFO hit{};
        hit.pt = point;
        ScreenToClient(input, &hit.pt);
        // 包含详细信息的子列，避免在文件所在行的其他列右键被当成背景。
        int index = (int)SendMessageW(input, LVM_SUBITEMHITTEST, 0, (LPARAM)&hit);
        return index < 0;
    }
    // 同一系统窗口内部也可能包含表头、项目和空白，使用系统可访问角色区分。
    ComPtr<IAccessible> accessible;
    VARIANT child{};
    HRESULT hr = ViewAccessibleAt(input, point, &accessible, &child);
    if (FAILED(hr) || !accessible) {
        VariantClear(&child);
        return std::nullopt;
    }
    for (int depth = 0; depth < 12; ++depth) {
        HWND owner = nullptr;
        if (FAILED(WindowFromAccessibleObject(accessible.Get(), &owner)) || !owner || !IsChild(host, owner))
            break;
        VARIANT role{};
        hr = accessible->get_accRole(child, &role);
        LONG value = SUCCEEDED(hr) && role.vt == VT_I4 ? role.lVal : 0;
        VariantClear(&role);
        if (value == ROLE_SYSTEM_LISTITEM || value == ROLE_SYSTEM_OUTLINEITEM || value == ROLE_SYSTEM_ROW) {
            VariantClear(&child);
            return false;
        }
        if (value == ROLE_SYSTEM_LIST || value == ROLE_SYSTEM_PANE || value == ROLE_SYSTEM_CLIENT) {
            VariantClear(&child);
            return true;
        }
        if (value != ROLE_SYSTEM_STATICTEXT && value != ROLE_SYSTEM_GRAPHIC && value != ROLE_SYSTEM_TEXT)
            break;
        if (child.vt == VT_I4 && child.lVal != CHILDID_SELF) {
            VariantClear(&child);
            child.vt = VT_I4;
            child.lVal = CHILDID_SELF;
            continue;
        }
        ComPtr<IDispatch> parent;
        if (FAILED(accessible->get_accParent(&parent)) || !parent)
            break;
        ComPtr<IAccessible> next;
        if (FAILED(parent.As(&next)))
            break;
        accessible = next;
        VariantClear(&child);
        child.vt = VT_I4;
        child.lVal = CHILDID_SELF;
    }
    VariantClear(&child);
    return std::nullopt;
}
LRESULT CALLBACK ShellView::Subclass(HWND w, UINT m, WPARAM a, LPARAM b, UINT_PTR key, DWORD_PTR data) {
    auto *s = (ShellView *)data;
    if (m == WM_KILLFOCUS)
        ++s->app.pasteGeneration;
    if (m == WM_XBUTTONUP) {
        s->app.Activate(s->app.model.PaneOf(s->id));
        s->app.Travel(GET_XBUTTON_WPARAM(a) == XBUTTON1 ? -1 : 1);
        return TRUE;
    }
    if (m == WM_CONTEXTMENU) {
        POINT p{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        bool keyboard = p.x == -1 && p.y == -1;
        HWND input = keyboard ? GetFocus() : WindowFromPoint(p);
        HWND source = (HWND)a;
        if (!keyboard) {
            POINT client = p;
            RECT bounds{};
            if (!input || !ScreenToClient(input, &client) || !GetClientRect(input, &bounds) ||
                !PtInRect(&bounds, client))
                return DefSubclassProc(w, m, a, b);
        }
        // 使用真实输入位置避免控件把消息转发到文件视图后被再次截获。
        // 未识别的内容、编辑器和表头保留自己的系统菜单。
        if (NativeControlMenu(source, s->host) || (source != s->host && !IsChild(s->host, source)))
            return DefSubclassProc(w, m, a, b);
        auto background = s->ContextMenuBackground(input, p, keyboard);
        if (!background)
            return DefSubclassProc(w, m, a, b);
        if (keyboard) {
            // 键盘菜单靠近当前项目，不使用可能位于另一栏的鼠标位置。
            HWND viewWindow = s->host;
            ComPtr<IFolderView> folder;
            int index = -1;
            PITEMID_CHILD item = nullptr;
            p = {16, 16};
            if (s->shell) {
                s->shell->GetWindow(&viewWindow);
                if (SUCCEEDED(s->shell.As(&folder)) && SUCCEEDED(folder->GetFocusedItem(&index)) &&
                    index >= 0 && SUCCEEDED(folder->Item(index, &item))) {
                    POINT location{};
                    if (SUCCEEDED(folder->GetItemPosition(item, &location)))
                        p = location;
                    CoTaskMemFree(item);
                }
            }
            ClientToScreen(viewWindow, &p);
        }
        s->app.FileMenu(*s, p, *background);
        return 0;
    }
    if (m == WM_SETFOCUS) {
        int p = s->app.model.PaneOf(s->id);
        if (p >= 0 && s->app.model.panes[p].selected == s->id && s->app.model.active != p) {
            s->app.model.active = p;
            PostMessageW(s->app.window, WM_ZEN_CHANGED, (WPARAM)s->id, 2);
        }
    }
    if (m == WM_DESTROY || m == WM_NCDESTROY) {
        // 导航会在 ShellView 本身仍存活时销毁旧子窗口，必须就地配对撤销。
        if (std::erase(s->drops, w)) {
            HRESULT hr = RevokeDragDrop(w);
            Log(L"撤销视图拖放 tab=" + std::to_wstring(s->id) + L" result=" +
                std::to_wstring((unsigned long)hr));
        }
    }
    if (m == WM_NCDESTROY) {
        std::erase(s->hooks, w);
        RemoveWindowSubclass(w, Subclass, key);
    }
    return DefSubclassProc(w, m, a, b);
}
HRESULT ShellView::DragEnter(IDataObject *d, DWORD key, POINTL p, DWORD *e) {
    if (!d || !e)
        return E_POINTER;
    DragLeave();
    dragging = d;
    Log(L"拖放进入 tab=" + std::to_wstring(id) + L" keys=" + std::to_wstring(key) + L" allowed=" +
        std::to_wstring(*e));
    dragButtons = key & (MK_LBUTTON | MK_RBUTTON);
    return DragOver(key, p, e);
}
ComPtr<IShellItem> ShellView::DropItem(POINTL point) {
    HWND hit = ViewWindowAt(host, {point.x, point.y});
    if (hit != host && !IsChild(host, hit))
        return {};
    wchar_t name[80]{};
    GetClassNameW(hit, name, 80);
    if (!wcscmp(name, L"SysListView32")) {
        LVHITTESTINFO info{};
        info.pt = {point.x, point.y};
        ScreenToClient(hit, &info.pt);
        int index = (int)SendMessageW(hit, LVM_HITTEST, 0, (LPARAM)&info);
        if (index >= 0 && (info.flags & LVHT_ONITEM)) {
            ComPtr<IFolderView2> view;
            ComPtr<IShellItem> item;
            if (shell && SUCCEEDED(shell.As(&view)) && SUCCEEDED(view->GetItem(index, IID_PPV_ARGS(&item))))
                return item;
            return {};
        }
    }
    if (!wcscmp(name, L"SysListView32"))
        return Current();
    auto background = ContextMenuBackground(hit, {point.x, point.y}, false);
    if (!background)
        return {};
    if (*background)
        return Current();
    // DirectUI 的项目没有独立 HWND。以系统可访问命中结果匹配视图中唯一项目；
    // 名称有歧义或命中无法确认时禁止放置，不能退回外层目录。
    ComPtr<IAccessible> accessible;
    VARIANT child{};
    if (FAILED(ViewAccessibleAt(hit, {point.x, point.y}, &accessible, &child))) {
        VariantClear(&child);
        return {};
    }
    std::wstring label;
    for (int depth = 0; accessible && depth < 12; ++depth) {
        VARIANT role{};
        accessible->get_accRole(child, &role);
        bool item =
            role.vt == VT_I4 && (role.lVal == ROLE_SYSTEM_LISTITEM || role.lVal == ROLE_SYSTEM_OUTLINEITEM ||
                                 role.lVal == ROLE_SYSTEM_ROW);
        VariantClear(&role);
        if (item) {
            BSTR text = nullptr;
            if (SUCCEEDED(accessible->get_accName(child, &text)) && text)
                label.assign(text, SysStringLen(text));
            SysFreeString(text);
            break;
        }
        if (child.vt == VT_I4 && child.lVal != CHILDID_SELF) {
            VariantClear(&child);
            child.vt = VT_I4;
            child.lVal = CHILDID_SELF;
            continue;
        }
        ComPtr<IDispatch> parent;
        if (FAILED(accessible->get_accParent(&parent)) || !parent)
            break;
        accessible.Reset();
        parent.As(&accessible);
        VariantClear(&child);
        child.vt = VT_I4;
        child.lVal = CHILDID_SELF;
    }
    VariantClear(&child);
    if (label.empty())
        return {};
    if (dragHitName == label)
        return dragHitItem;
    dragHitName = label;
    Log(L"拖放命中名称 tab=" + std::to_wstring(id) + L" name=" + label);
    dragHitItem.Reset();
    ComPtr<IFolderView2> folder;
    int count = 0;
    if (!shell || FAILED(shell.As(&folder)) || FAILED(folder->ItemCount(SVGIO_ALLVIEW, &count)))
        return {};
    for (int i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(folder->GetItem(i, IID_PPV_ARGS(&item)))) {
            dragHitItem.Reset();
            return {};
        }
        if (ItemName(item.Get(), SIGDN_NORMALDISPLAY) == label) {
            if (dragHitItem) {
                dragHitItem.Reset();
                return {};
            }
            dragHitItem = item;
        }
    }
    return dragHitItem;
}
HRESULT ShellView::DragOver(DWORD key, POINTL point, DWORD *effect) {
    if (!effect)
        return E_POINTER;
    if (!dragging) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    if (key & (MK_LBUTTON | MK_RBUTTON))
        dragButtons = key & (MK_LBUTTON | MK_RBUTTON);
    if ((dragButtons & MK_RBUTTON) || navigating) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    auto destination = DropItem(point);
    int order = 1;
    bool same = destination && dragDestination &&
                SUCCEEDED(destination->Compare(dragDestination.Get(), SICHINT_CANONICAL, &order)) && !order;
    HRESULT hr = S_OK;
    if (!same) {
        if (dragTarget)
            dragTarget->DragLeave();
        dragTarget.Reset();
        dragDestination = destination;
        if (destination)
            Log(L"拖放目标 tab=" + std::to_wstring(id) + L" path=" + ItemName(destination.Get()));
        hr = SystemDropTarget(destination.Get(), &dragTarget);
        if (SUCCEEDED(hr))
            hr = dragTarget->DragEnter(dragging.Get(), key, point, effect);
    } else if (dragTarget)
        hr = dragTarget->DragOver(key, point, effect);
    if (FAILED(hr) || !dragTarget || (*effect != DROPEFFECT_COPY && *effect != DROPEFFECT_MOVE))
        *effect = DROPEFFECT_NONE;
    return S_OK;
}
HRESULT ShellView::DragLeave() {
    if (dragTarget)
        dragTarget->DragLeave();
    dragTarget.Reset();
    dragDestination.Reset();
    dragging.Reset();
    dragButtons = 0;
    dragHitName.clear();
    dragHitItem.Reset();
    return S_OK;
}
HRESULT ShellView::Drop(IDataObject *d, DWORD key, POINTL p, DWORD *e) {
    if (!d || !e)
        return E_POINTER;
    key |= dragButtons;
    // 松开时重新核实落点，避免使用悬停期间目录刷新前的缓存。
    dragHitName.clear();
    dragHitItem.Reset();
    app.FileDrop(*this, d, key, p, e);
    DragLeave();
    return S_OK;
}
} // namespace zen
