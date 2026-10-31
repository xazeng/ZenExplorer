#pragma once
#include "app.h"
namespace zen {
struct TabHit {
    RECT rect{};
    Id tab = 0, group = 0;
    bool plus = false;
};
class TabStrip : public IDropTarget {
    LONG refs = 1;
    bool dragging = false, closePressed = false;
    HWND ghost{};
    int dropPane = -1, dropIndex = 0;
    Id dropGroup = 0;
    RECT gap{};
    std::vector<TabHit> naturalHits;
    void UpdateDrag(POINT);
    void EndDrag();
    void ResolveDrop(POINT, int &, int &, Id &);
    RECT CloseRect(const TabHit &) const;
    POINT down{};
    TabHit source{};
    Id hoverTab = 0, hoverGroup = 0;
    ULONGLONG hoverTime = 0;
    std::vector<GroupExpansionState> expanded;
    ComPtr<IDataObject> externalData;
    ULONGLONG releasedAt = 0;

  public:
    App &app;
    int pane;
    HWND hwnd{}, tooltip{};
    std::wstring tipText;
    int scroll = 0, total = 0, height = 34;
    std::vector<TabHit> hits;
    POINT mouse{-1, -1};
    bool external = false;
    TabStrip(App &, int);
    ~TabStrip();
    void Create();
    int Arrange(int width);
    void Paint();
    void RefreshDragFont(HFONT font);
    TabHit Hit(POINT) const;
    void EnsureVisible(Id);
    void RestoreExpanded();
    void FinishExternal(bool success);
    std::vector<GroupExpansionState> DetachExternal();
    void Hover(POINT screen, bool files);
    void FinishMove(POINT);
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Message(UINT, WPARAM, LPARAM);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return InterlockedDecrement(&refs);
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL, DWORD *) override;
};
} // namespace zen
