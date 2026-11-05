#pragma once
#include "model.h"
namespace zen {
class App;
// 每栏独立持有编辑会话、系统补全和控件生命周期；导航仍由 App 统一提交。
class AddressBar {
  public:
    AddressBar(App &owner, int paneIndex) : app(owner), pane(paneIndex) {}
    ~AddressBar();
    AddressBar(const AddressBar &) = delete;
    AddressBar &operator=(const AddressBar &) = delete;
    HRESULT Create(HWND first, HWND last);
    HWND EditWindow() const {
        return edit;
    }
    HWND CrumbWindow() const {
        return crumb;
    }
    bool CompletionReady() const {
        return complete && dropDown;
    }
    std::wstring Text() const;
    void Update();
    void Edit();
    void PaintBorder(HDC dc, HWND child) const;

  private:
    App &app;
    int pane;
    HWND edit{}, crumb{}, firstNav{}, lastNav{};
    ComPtr<IAutoComplete2> complete;
    ComPtr<IAutoCompleteDropDown> dropDown;
    struct EditSession {
        Id tab = 0;
        LocationKey path;
        std::wstring text;
    } session;
    struct Link {
        RECT rect, arrow;
        LocationKey path;
    };
    POINT hover{-1, -1};
    std::vector<Link> links;
    void ReleaseCompletion();
    void ShowFolders(HWND owner, Id tab, const Link &link);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK CrumbProc(HWND, UINT, WPARAM, LPARAM);
};
} // namespace zen
