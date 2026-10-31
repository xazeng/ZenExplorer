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
std::vector<std::pair<std::wstring, std::wstring>> Breadcrumbs(const std::wstring &path);
bool Confirm(HWND owner, const std::wstring &text);
std::optional<std::wstring> Input(HWND owner, const std::wstring &title, const std::wstring &value,
                                  const std::wstring &hint = L"");
} // namespace zen
