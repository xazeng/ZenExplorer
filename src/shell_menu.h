#pragma once
#include "platform.h"
namespace zen {
// 菜单句柄及动态扩展必须存活到 InvokeCommand 返回之后。
class ShellMenu {
    ComPtr<IObjectWithSite> attachedSite;
    // 执行目录来自菜单所属视图，不使用进程工作目录或另一栏的活动目录。
    std::wstring directory;
    std::string directoryAnsi;

  public:
    ComPtr<IContextMenu> context;
    ComPtr<IContextMenu2> menu2;
    ComPtr<IContextMenu3> menu3;
    HMENU menu{};
    HWND owner{};
    ShellMenu() = default;
    ShellMenu(const ShellMenu &) = delete;
    ShellMenu &operator=(const ShellMenu &) = delete;
    ~ShellMenu();
    HRESULT Create(IShellView *view, bool background, HWND window, UINT flags = CMF_NORMAL);
    std::wstring Verb(UINT command) const;
    UINT Find(const std::wstring &verb) const;
    HRESULT Invoke(UINT command, POINT point = {}, std::optional<DWORD> modifiers = std::nullopt);
};
} // namespace zen
