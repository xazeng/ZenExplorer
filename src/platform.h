#pragma once
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <optional>
#include <cstdint>
namespace zen {
using Microsoft::WRL::ComPtr;
inline constexpr wchar_t Home[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
std::filesystem::path DataDir();
std::filesystem::path ConfigDir();
std::filesystem::path LogDir();
std::filesystem::path ExeDir();
void Log(const std::wstring &message);
// 仅在显式启用文件日志时采样；不记录右键菜单停留时间为扩展执行耗时。
class ResourceTrace {
    std::wstring phase;
    ULONGLONG start = 0;
    DWORD handles = 0, gdi = 0, user = 0;
    bool enabled = false;

  public:
    explicit ResourceTrace(std::wstring phase);
    ~ResourceTrace();
};
std::wstring ErrorText(HRESULT hr);
std::wstring ItemName(IShellItem *item, SIGDN type = SIGDN_DESKTOPABSOLUTEPARSING);
ComPtr<IShellItem> ParseItem(const std::wstring &path);
// 产品导航边界；实际是否存在父级由 ShellView 的导航缓存确认。
bool CanNavigateUp(const std::wstring &path);
std::wstring LocationName(const std::wstring &path);
std::wstring SizeText(uint64_t size);
std::wstring Quote(const std::wstring &text);
std::wstring WindowText(HWND hwnd);
void Report(HWND owner, const std::wstring &text, HRESULT hr = S_OK);
enum class LocalPathState { Deferred, Exists, Missing, Unknown };
LocalPathState InspectLocalPath(const std::wstring &path);
void DrawSystemGlyph(HDC dc, RECT rect, wchar_t glyph, UINT dpi, COLORREF color);
HICON SystemImageIcon(int index, int size);
std::vector<std::pair<std::wstring, std::wstring>> Breadcrumbs(const std::wstring &path);
// 按需枚举 Shell 直属子目录；失败不返回部分列表，不读取文件内容。
HRESULT ChildFolders(const std::wstring &path, std::vector<std::pair<std::wstring, std::wstring>> &out);
HRESULT ChildFolderItems(IShellItem *parent, std::vector<ComPtr<IShellItem>> &out);
// 由完整 PIDL 生成当前项到此电脑/桌面的层级，不依赖扩展的 GetParent。
HRESULT ShellHierarchy(IShellItem *item, std::vector<ComPtr<IShellItem>> &out);
bool ValidPidl(const std::vector<BYTE> &bytes);
bool Confirm(HWND owner, const std::wstring &text);
std::optional<std::wstring> Input(HWND owner, const std::wstring &title, const std::wstring &value,
                                  const std::wstring &hint = L"");
} // namespace zen
