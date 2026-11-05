#pragma once
#include "storage.h"
#include "address_bar.h"
#include "task_feedback.h"
#include <shldisp.h>
#include <unordered_map>
namespace zen {
class ShellView;
class TabStrip;
inline constexpr UINT WM_ZEN_NAV = WM_APP + 10, WM_ZEN_CHANGED = WM_APP + 11, WM_ZEN_ACTION = WM_APP + 12;
struct NavRequest {
    Id id;
    LocationKey path;
    bool newTab = false;
    ComPtr<IShellItem> item;
};
class App {
  public:
    HINSTANCE instance{};
    HWND window{}, toolbar{}, status{}, error[2]{};
    HMENU mainMenu{};
    HWND menuReturnFocus{};
    bool menuOpen = false;
    int menuIndex = 0, nextMenu = -1;
    HMENU selectedMenu{};
    UINT selectedMenuFlags = 0;
    void ShowMainMenu(int index);
    std::unique_ptr<AddressBar> addressBars[2];
    HWND AddressEdit(int p) const {
        return addressBars[p] ? addressBars[p]->EditWindow() : nullptr;
    }
    HWND AddressCrumb(int p) const {
        return addressBars[p] ? addressBars[p]->CrumbWindow() : nullptr;
    }
    HFONT font{};
    HICON folderIcon{}, computerIcon{}, missingIcon{}, windowLargeIcon{}, windowSmallIcon{};
    UINT dpi = 96;
    bool customFrame = false;
    bool windowActive = false;
    int TitleOffset() const;
    int TitleHeight() const;
    void UpdateFrame();
    RECT viewBounds[2]{};
    Model model;
    std::unique_ptr<TabStrip> strips[2];
    std::unordered_map<Id, std::unique_ptr<ShellView>> views;
    LocationKey RememberLocation(IShellItem *item);
    void PruneLocations();
    bool readOnly = false, dirty = false, closing = false, sizing = false, restoring = false;
    int saveFailures = 0;
    ComPtr<IContextMenu2> menu2;
    ComPtr<IContextMenu3> menu3;
    std::unordered_map<Id, HICON> icons;
    std::unordered_map<Id, HICON> homeIcons;
    std::unordered_map<Id, int> homeIconIndices;
    std::unordered_map<Id, LocationKey> homeIconPaths;
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
    void Layout(bool updateStatus = true, int pane = -1);
    void RedrawPaneFocus();
    void Activate(int p, Id id = 0, bool focus = false);
    void EnsureView(Id id);
    void Changed();
    void Save();
    void UpdateStatus();
    void UpdateAddress(int p);
    std::wstring AddressText(int p) const;
    void BuildMenu();
    void Command(UINT id);
    void Navigate(Id id, const LocationKey &path, bool append = true, int historyIndex = -1);
    void Travel(int delta);
    void NewTab(int pane, const LocationKey &path = Home, Id group = 0, int index = -1, Id opener = 0);
    void CloseTabs(int p, const std::vector<Id> &ids, bool group = false);
    enum class GroupInteraction { User, DragPreview };
    void SetGroupCollapsed(int pane, Id group, bool collapsed,
                           GroupInteraction source = GroupInteraction::User);
    void ToggleGroup(int pane, Id group);
    void RestoreGroups(const std::vector<GroupExpansionState> &states);
    void MoveTab(Id tab, int target, int index, Id group, bool keepExpanded);
    void MoveGroup(int pane, Id group, int target, int index);
    void SyncViews(int pane = -1);
    void TabMenu(int p, Id tab, Id group, POINT pt);
    void FileMenu(ShellView &, POINT, bool background);
    FileTaskFeedback tasks;
    uint64_t pasteGeneration = 0;
    uint64_t dropGeneration = 0;
    void Paste(ShellView &, IShellItem *destination = nullptr);
    HRESULT ShellVerb(ShellView &, const std::wstring &, bool background,
                      std::optional<DWORD> modifiers = std::nullopt);
    void DeleteSelection(ShellView &, bool permanent);
    void CopySelection(ShellView &, bool cut);
    void OpenSelected(ShellView &, bool newTab);
    void OpenInExplorer(const LocationKey &path);
    void FileDrop(ShellView &, IDataObject *, DWORD, POINTL, DWORD *);
    bool Key(MSG &);
    void ViewMouse(const MSG &);
    void EditAddress(int p);
    void ShowError(Id id, HRESULT hr);
    std::wstring Title(const Tab &) const;
    HICON Icon(Id id) const;
    HICON TabIcon(Id id) const;
    void RefreshHomeIcon(Id id);
    void RefreshLocalIcon(Id id);
    void RefreshMetadata(Id id, IShellItem *item);
};
} // namespace zen
