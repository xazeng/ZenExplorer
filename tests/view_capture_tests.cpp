#include "app.h"
#include "shell_view.h"
#include <iostream>
#include <new>
#include <wrl/implements.h>
using namespace zen;
namespace {
// 不创建浏览器窗口；只模拟采集接口与同步 COM 重入，确定失败和通知的先后次序。
class CaptureShell final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IShellView, IFolderView2, IColumnManager, IPersistFolder2> {
  public:
    PIDLIST_ABSOLUTE folder = nullptr;
    FOLDERVIEWMODE mode = FVM_ICON;
    int iconSize = 48, captures = 0;
    HRESULT modeResult = S_OK, sortResult = S_OK, folderResult = S_OK;
    int failedColumn = -1;
    std::vector<SORTCOLUMN> sort{{PKEY_Size, SORT_DESCENDING}};
    std::vector<Column> columns{{PKEY_ItemNameDisplay, 280}, {PKEY_Size, 100}};
    std::function<void()> callback;
    ~CaptureShell() {
        CoTaskMemFree(folder);
    }
    HRESULT STDMETHODCALLTYPE GetViewModeAndIconSize(FOLDERVIEWMODE *outMode, int *outSize) override {
        ++captures;
        if (callback) {
            auto invoke = std::move(callback);
            callback = {};
            invoke();
        }
        if (FAILED(modeResult))
            return modeResult;
        *outMode = mode;
        *outSize = iconSize;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetViewModeAndIconSize(FOLDERVIEWMODE modeValue, int size) override {
        mode = modeValue;
        iconSize = size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSortColumnCount(int *count) override {
        *count = static_cast<int>(sort.size());
        return sortResult;
    }
    HRESULT STDMETHODCALLTYPE GetSortColumns(SORTCOLUMN *values, int count) override {
        if (count != static_cast<int>(sort.size()))
            return E_INVALIDARG;
        std::copy(sort.begin(), sort.end(), values);
        return sortResult;
    }
    HRESULT STDMETHODCALLTYPE SetSortColumns(const SORTCOLUMN *values, int count) override {
        sort.assign(values, values + count);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFolder(REFIID iid, void **out) override {
        *out = nullptr;
        return FAILED(folderResult) ? folderResult : QueryInterface(iid, out);
    }
    HRESULT STDMETHODCALLTYPE GetCurFolder(PIDLIST_ABSOLUTE *out) override {
        *out = ILCloneFull(folder);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID *out) override {
        *out = CLSID_NULL;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetColumnCount(CM_ENUM_FLAGS, UINT *count) override {
        *count = static_cast<UINT>(columns.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetColumns(CM_ENUM_FLAGS, PROPERTYKEY *keys, UINT count) override {
        if (count != columns.size())
            return E_INVALIDARG;
        for (UINT i = 0; i < count; ++i)
            keys[i] = columns[i].key;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetColumnInfo(REFPROPERTYKEY key, CM_COLUMNINFO *info) override {
        for (size_t i = 0; i < columns.size(); ++i)
            if (IsEqualPropertyKey(key, columns[i].key)) {
                if (failedColumn >= 0 && i == static_cast<size_t>(failedColumn))
                    return E_FAIL;
                info->uWidth = columns[i].width;
                return S_OK;
            }
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE SetColumnInfo(REFPROPERTYKEY key, const CM_COLUMNINFO *info) override {
        for (auto &column : columns)
            if (IsEqualPropertyKey(key, column.key)) {
                column.width = info->uWidth;
                return S_OK;
            }
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE SetColumns(const PROPERTYKEY *keys, UINT count) override {
        std::vector<Column> reordered;
        for (UINT i = 0; i < count; ++i) {
            auto found = std::find_if(columns.begin(), columns.end(), [&](const Column &column) {
                return IsEqualPropertyKey(keys[i], column.key);
            });
            if (found == columns.end())
                return E_INVALIDARG;
            reordered.push_back(*found);
        }
        columns = std::move(reordered);
        return S_OK;
    }

#define UNSUPPORTED_CAPTURE_METHOD(name, ...)                                                                \
    HRESULT STDMETHODCALLTYPE name(__VA_ARGS__) override {                                                   \
        return E_NOTIMPL;                                                                                    \
    }
    UNSUPPORTED_CAPTURE_METHOD(GetWindow, HWND *)
    UNSUPPORTED_CAPTURE_METHOD(ContextSensitiveHelp, BOOL)
    UNSUPPORTED_CAPTURE_METHOD(TranslateAccelerator, MSG *)
    UNSUPPORTED_CAPTURE_METHOD(EnableModeless, BOOL)
    UNSUPPORTED_CAPTURE_METHOD(UIActivate, UINT)
    UNSUPPORTED_CAPTURE_METHOD(Refresh)
    UNSUPPORTED_CAPTURE_METHOD(CreateViewWindow, IShellView *, LPCFOLDERSETTINGS, IShellBrowser *, RECT *,
                               HWND *)
    UNSUPPORTED_CAPTURE_METHOD(DestroyViewWindow)
    UNSUPPORTED_CAPTURE_METHOD(GetCurrentInfo, LPFOLDERSETTINGS)
    UNSUPPORTED_CAPTURE_METHOD(AddPropertySheetPages, DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM)
    UNSUPPORTED_CAPTURE_METHOD(SaveViewState)
    UNSUPPORTED_CAPTURE_METHOD(SelectItem, PCUITEMID_CHILD, SVSIF)
    UNSUPPORTED_CAPTURE_METHOD(GetItemObject, UINT, REFIID, void **)
    UNSUPPORTED_CAPTURE_METHOD(GetCurrentViewMode, UINT *)
    UNSUPPORTED_CAPTURE_METHOD(SetCurrentViewMode, UINT)
    UNSUPPORTED_CAPTURE_METHOD(Item, int, PITEMID_CHILD *)
    UNSUPPORTED_CAPTURE_METHOD(ItemCount, UINT, int *)
    UNSUPPORTED_CAPTURE_METHOD(Items, UINT, REFIID, void **)
    UNSUPPORTED_CAPTURE_METHOD(GetSelectionMarkedItem, int *)
    UNSUPPORTED_CAPTURE_METHOD(GetFocusedItem, int *)
    UNSUPPORTED_CAPTURE_METHOD(GetItemPosition, PCUITEMID_CHILD, POINT *)
    UNSUPPORTED_CAPTURE_METHOD(GetSpacing, POINT *)
    UNSUPPORTED_CAPTURE_METHOD(GetDefaultSpacing, POINT *)
    UNSUPPORTED_CAPTURE_METHOD(GetAutoArrange)
    UNSUPPORTED_CAPTURE_METHOD(SelectItem, int, DWORD)
    UNSUPPORTED_CAPTURE_METHOD(SelectAndPositionItems, UINT, PCUITEMID_CHILD_ARRAY, POINT *, DWORD)
    UNSUPPORTED_CAPTURE_METHOD(SetGroupBy, REFPROPERTYKEY, BOOL)
    UNSUPPORTED_CAPTURE_METHOD(GetGroupBy, PROPERTYKEY *, BOOL *)
    UNSUPPORTED_CAPTURE_METHOD(SetViewProperty, PCUITEMID_CHILD, REFPROPERTYKEY, REFPROPVARIANT)
    UNSUPPORTED_CAPTURE_METHOD(GetViewProperty, PCUITEMID_CHILD, REFPROPERTYKEY, PROPVARIANT *)
    UNSUPPORTED_CAPTURE_METHOD(SetTileViewProperties, PCUITEMID_CHILD, LPCWSTR)
    UNSUPPORTED_CAPTURE_METHOD(SetExtendedTileViewProperties, PCUITEMID_CHILD, LPCWSTR)
    UNSUPPORTED_CAPTURE_METHOD(SetText, FVTEXTTYPE, LPCWSTR)
    UNSUPPORTED_CAPTURE_METHOD(SetCurrentFolderFlags, DWORD, DWORD)
    UNSUPPORTED_CAPTURE_METHOD(GetCurrentFolderFlags, DWORD *)
    UNSUPPORTED_CAPTURE_METHOD(GetItem, int, REFIID, void **)
    UNSUPPORTED_CAPTURE_METHOD(GetVisibleItem, int, BOOL, int *)
    UNSUPPORTED_CAPTURE_METHOD(GetSelectedItem, int, int *)
    UNSUPPORTED_CAPTURE_METHOD(GetSelection, BOOL, IShellItemArray **)
    UNSUPPORTED_CAPTURE_METHOD(GetSelectionState, PCUITEMID_CHILD, DWORD *)
    UNSUPPORTED_CAPTURE_METHOD(InvokeVerbOnSelection, LPCSTR)
    UNSUPPORTED_CAPTURE_METHOD(SetGroupSubsetCount, UINT)
    UNSUPPORTED_CAPTURE_METHOD(GetGroupSubsetCount, UINT *)
    UNSUPPORTED_CAPTURE_METHOD(SetRedraw, BOOL)
    UNSUPPORTED_CAPTURE_METHOD(IsMoveInSameFolder)
    UNSUPPORTED_CAPTURE_METHOD(DoRename)
    UNSUPPORTED_CAPTURE_METHOD(Initialize, PCIDLIST_ABSOLUTE)
#undef UNSUPPORTED_CAPTURE_METHOD
};
} // namespace

bool CheckViewCapture() {
    bool ok = true;
    auto expect = [&](bool value, const char *message) {
        if (!value) {
            std::cerr << message << "\n";
            ok = false;
        }
    };
    App app;
    // 通知发给本测试专用的消息窗口，不进入其他 Shell 回归的消息队列。
    app.window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!app.window) {
        expect(false, "could not create capture notification target");
        return false;
    }
    struct CloseWindow {
        HWND window;
        ~CloseWindow() {
            DestroyWindow(window);
        }
    } closeWindow{app.window};
    auto fake = Microsoft::WRL::Make<CaptureShell>();
    if (!fake)
        return false;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_Windows, 0, nullptr, &fake->folder))) {
        expect(false, "could not obtain filesystem identity for capture test");
        return false;
    }
    const Id id = app.model.panes[0].selected;
    auto owned = std::make_unique<ShellView>(app, id);
    owned->shell = fake;
    app.views[id] = std::move(owned);
    auto &view = *app.views.at(id);
    auto *tab = app.model.Find(id);
    expect(view.NeedsCapture() && view.Capture() && !view.NeedsCapture(),
           "initial view capture did not become clean");
    expect(tab->viewMode == FVM_ICON && tab->iconSize == 48 && tab->sort.size() == 1 &&
               IsEqualPropertyKey(tab->sort[0].propkey, PKEY_Size) &&
               tab->sort[0].direction == SORT_DESCENDING && tab->columns.size() == 2 &&
               tab->columns[0].width == 280 && tab->columns[1].width == 100,
           "capture did not preserve mode, sorting and widths");

    view.OnStateChange(nullptr, CDBOSC_SELCHANGE);
    view.OnStateChange(nullptr, CDBOSC_KILLFOCUS);
    view.OnStateChange(nullptr, CDBOSC_SETFOCUS);
    expect(!view.NeedsCapture(), "selection or focus incorrectly dirtied view settings");
    view.host =
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 10, 10, app.window, nullptr, nullptr, nullptr);
    expect(view.host && SetWindowSubclass(view.host, ShellView::Subclass, reinterpret_cast<UINT_PTR>(&view),
                                          reinterpret_cast<DWORD_PTR>(&view)),
           "could not install view input capture hook");
    if (view.host) {
        SendMessageW(view.host, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        expect(!view.NeedsCapture(), "plain scrolling dirtied view settings");
        SendMessageW(view.host, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), 0);
        expect(view.NeedsCapture() && view.Capture() && !view.NeedsCapture(),
               "control-wheel mode change did not request capture");
    }
    view.OnColumnClicked(nullptr, 0);
    fake->sort[0].direction = SORT_ASCENDING;
    std::reverse(fake->columns.begin(), fake->columns.end());
    expect(view.NeedsCapture() && view.Capture() && !view.NeedsCapture() && tab->sort.size() == 1 &&
               tab->columns.size() == 2 && tab->sort[0].direction == SORT_ASCENDING &&
               IsEqualPropertyKey(tab->columns[0].key, PKEY_Size),
           "column notification did not capture changed sort and order");

    tab->viewMode = FVM_DETAILS;
    tab->iconSize = 16;
    expect(SUCCEEDED(view.ApplyViewMode()) && view.NeedsCapture() && view.Capture() &&
               tab->viewMode == FVM_DETAILS && tab->iconSize == 16 && tab->sort.size() == 1 &&
               tab->columns.size() == 2 && tab->sort[0].direction == SORT_ASCENDING &&
               tab->columns[0].width == 100,
           "mode-only apply lost other settings or did not request capture");

    fake->columns[0].width = 150;
    fake->columns[1].width = 350;
    fake->failedColumn = 1;
    expect(!view.Capture() && view.NeedsCapture() && tab->columns.size() == 2 &&
               tab->columns[0].width == 100 && tab->columns[1].width == 280,
           "failed column query committed partial columns or cleared pending capture");
    fake->failedColumn = -1;
    expect(view.Capture() && !view.NeedsCapture() && tab->columns.size() == 2 &&
               tab->columns[0].width == 150 && tab->columns[1].width == 350,
           "column capture did not recover after temporary failure");

    fake->modeResult = E_FAIL;
    expect(!view.Capture() && view.NeedsCapture(), "forced capture failure was marked clean");
    fake->modeResult = fake->sortResult = fake->folderResult = E_NOTIMPL;
    expect(view.Capture() && !view.NeedsCapture() && tab->viewMode == FVM_DETAILS && tab->columns.size() == 2,
           "unsupported provider interfaces caused retries or erased saved settings");
    fake->modeResult = fake->sortResult = fake->folderResult = S_OK;

    fake->mode = FVM_TILE;
    fake->callback = [&] { view.MarkSettingsChanged(); };
    expect(!view.Capture() && view.NeedsCapture() && tab->viewMode == FVM_DETAILS,
           "reentrant settings change committed an obsolete sample");
    expect(view.Capture() && !view.NeedsCapture() && tab->viewMode == FVM_TILE,
           "new settings revision was not captured on retry");

    int before = fake->captures;
    fake->callback = [&] { expect(!view.Capture(), "nested capture was not rejected"); };
    expect(view.Capture() && !view.NeedsCapture() && fake->captures == before + 1,
           "nested capture recursively called provider or blocked later capture");

    fake->callback = [] { throw std::bad_alloc{}; };
    bool threw = false;
    try {
        view.Capture();
    } catch (const std::bad_alloc &) {
        threw = true;
    }
    expect(threw && view.NeedsCapture() && view.Capture() && !view.NeedsCapture(),
           "capture exception retained its reentrancy guard");

    fake->mode = FVM_LIST;
    fake->callback = [&] { app.views.erase(id); };
    expect(!view.Capture() && !app.views.contains(id) && tab->viewMode == FVM_TILE,
           "capture committed settings after reentrant view destruction");

    auto navigating = std::make_unique<ShellView>(app, id);
    navigating->shell = fake;
    app.views[id] = std::move(navigating);
    fake->callback = [&] { app.views.erase(id); };
    expect(app.views.at(id)->OnNavigationPending(fake->folder) == E_ABORT && !app.views.contains(id) &&
               tab->viewMode == FVM_TILE,
           "system navigation continued after capture closed its source view");
    return ok;
}
