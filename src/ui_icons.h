#pragma once
#include "platform.h"
namespace zen {
enum class UiIcon { Back, Forward, Up, Refresh, Add, ChevronRight };
// 使用已确认的 16 DIP 矢量图形；disabled 只改变颜色。
void DrawUiIcon(HDC dc, RECT bounds, UiIcon icon, UINT dpi, bool disabled = false);
// 常用标签星标：bounds 为系统图标右下角的 12×12 DIP 区域，不改变底图。
void DrawFavoriteBadge(HDC dc, RECT bounds);
} // namespace zen
