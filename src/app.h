#pragma once
#include "storage.h"
#include <shldisp.h>
#include <unordered_map>
namespace zen {
class ShellView;
class TabStrip;
inline constexpr UINT WM_ZEN_NAV = WM_APP + 10, WM_ZEN_CHANGED = WM_APP + 11, WM_ZEN_ACTION = WM_APP + 12;
struct NavRequest {
    Id id;
    std::wstring path;
    bool newTab = false;
};
class App {
  public:
    HINSTANCE instance{};
    HWND window{}, toolbar{}, status{}, address[2]{}, crumb[2]{}, error[2]{};
    HMENU mainMenu{};
    HWND menuReturnFocus{};
    bool menuOpen = false;
    int menuIndex = 0, nextMenu = -1;
    HMENU selectedMenu{};
    UINT selectedMenuFlags = 0;
    void ShowMainMenu(int index);
    ComPtr<IAutoComplete2> addressComplete[2];
    ComPtr<IAutoCompleteDropDown> addressDropDown[2];
    HFONT font{};
    HICON folderIcon{}, computerIcon{}, missingIcon{}, windowLargeIcon{}, windowSmallIcon{};
    UINT dpi = 96;
    bool customFrame = false;
    int TitleOffset() const;
    int TitleHeight() const;
    void UpdateFrame();
    RECT viewBounds[2]{};
    Model model;
    std::unique_ptr<TabStrip> strips[2];
    std::unordered_map<Id, std::unique_ptr<ShellView>> views;
    bool readOnly = false, dirty = false, closing = false, sizing = false, restoring = false;
    int saveFailures = 0;
    ComPtr<IContextMenu2> menu2;
    ComPtr<IContextMenu3> menu3;
    std::unordered_map<Id, HICON> icons;
    std::unordered_map<Id, int> iconIndices;
    App();
    ~App();
    int Run(HINSTANCE, int, const std::wstring &);
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Message(UINT, WPARAM, LPARAM);
    int S(int x) const {
        return MulDiv(x, dpi, 96);
    }
    void RefreshWindowIcons();
    void RefreshDpiResources();
    void Layout(bool updateStatus = true);
    void Activate(int p, Id id = 0, bool focus = false);
    void EnsureView(Id id);
    void Changed();
    void Save();
    void UpdateStatus();
    void UpdateAddress(int p);
    void BuildMenu();
    void Command(UINT id);
    void Navigate(Id id, const std::wstring &path, bool append = true);
    void Travel(int delta);
    void NewTab(int pane, const std::wstring &path = Home, Id group = 0, int index = -1, Id opener = 0);
    void CloseTabs(int p, const std::vector<Id> &ids, bool group = false);
    void SyncViews();
    void TabMenu(int p, Id tab, Id group, POINT pt);
    void FileMenu(ShellView &, POINT, bool background);
    struct PendingPaste {
        std::filesystem::path result;
        DWORD clipboardSequence;
        bool move = true;
        Id tab = 0;
        int pane = -1;
        uint64_t generation = 0;
        HWND focus = nullptr;
        std::wstring destination;
        std::vector<std::wstring> created;
        bool read = false;
        ULONGLONG deadline = 0;
        // 仅用于观察退出；关闭句柄绝不终止工作进程。
        std::shared_ptr<void> process;
    };
    std::vector<PendingPaste> pendingPastes;
    struct PendingDrop {
        std::filesystem::path result;
        std::shared_ptr<void> process;
        std::vector<GroupExpansionState> expanded;
        uint64_t generation = 0;
    };
    std::vector<PendingDrop> pendingDrops;
    struct RetiredTask {
        std::filesystem::path result;
        std::shared_ptr<void> process;
    };
    std::vector<RetiredTask> retiredTasks;
    uint64_t pasteGeneration = 0;
    uint64_t dropGeneration = 0;
    bool PasteFeedbackCurrent(const PendingPaste &, HWND foreground, HWND focus);
    void PollPastes();
    void Paste(ShellView &, IShellItem *destination = nullptr);
    HRESULT ShellVerb(ShellView &, const std::wstring &, bool background,
                      std::optional<DWORD> modifiers = std::nullopt);
    void DeleteSelection(ShellView &, bool permanent);
    void CopySelection(ShellView &, bool cut);
    void OpenSelected(ShellView &, bool newTab);
    void OpenInExplorer(const std::wstring &path);
    void FileDrop(ShellView &, IDataObject *, DWORD, POINTL, DWORD *);
    bool Key(MSG &);
    void ViewMouse(const MSG &);
    void EditAddress(int p);
    void ShowError(Id id, HRESULT hr);
    std::wstring Title(const Tab &) const;
    HICON Icon(Id id) const;
    void RefreshLocalIcon(Id id);
    void RefreshMetadata(Id id, IShellItem *item);
};
} // namespace zen
