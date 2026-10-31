#pragma once
#include "platform.h"
namespace zen {
enum class UiIcon { Back, Forward, Up, Refresh, Add };
// 使用已确认的 16 DIP 矢量图形；disabled 只改变颜色。
void DrawUiIcon(HDC dc, RECT bounds, UiIcon icon, UINT dpi, bool disabled = false);
} // namespace zen
