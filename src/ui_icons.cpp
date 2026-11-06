#include "ui_icons.h"
#include <gdiplus.h>
namespace zen {
static constexpr COLORREF NavigationIconColor = RGB(80, 80, 80);
static constexpr COLORREF DisabledNavigationIconColor = RGB(210, 210, 210);
void DrawHomeBadge(HDC dc, RECT bounds) {
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return;
    Gdiplus::Graphics graphics(dc);
    RECT clip{};
    GetClipBox(dc, &clip);
    graphics.SetClip(Gdiplus::Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
    graphics.SetClip(
        Gdiplus::Rect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top),
        Gdiplus::CombineModeIntersect);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.TranslateTransform(static_cast<float>(bounds.left), static_cast<float>(bounds.top));
    graphics.ScaleTransform((bounds.right - bounds.left) / 8.0f, (bounds.bottom - bounds.top) / 8.0f);
    // 浅色隔离边避免小房子的轮廓与底层系统图标混在一起。
    const Gdiplus::PointF points[] = {{0.9f, 3.6f}, {4.0f, 1.0f}, {7.1f, 3.6f},
                                    {6.4f, 3.6f}, {6.4f, 6.8f}, {1.6f, 6.8f}, {1.6f, 3.6f}};
    Gdiplus::GraphicsPath house;
    house.AddPolygon(points, 7);
    Gdiplus::Pen separator(Gdiplus::Color(255, 255, 255, 255), 1.8f);
    separator.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&separator, &house);
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, 255, 255, 255));
    graphics.FillPath(&fill, &house);
    COLORREF ink = NavigationIconColor;
    Gdiplus::Pen outline(Gdiplus::Color(255, GetRValue(ink), GetGValue(ink), GetBValue(ink)), 0.8f);
    outline.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&outline, &house);
    const Gdiplus::PointF door[] = {{3.2f, 6.8f}, {3.2f, 4.6f}, {4.8f, 4.6f}, {4.8f, 6.8f}};
    graphics.DrawLines(&outline, door, 4);
}
void DrawUiIcon(HDC dc, RECT bounds, UiIcon icon, UINT dpi, bool disabled) {
    if (!dc || !dpi)
        return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.TranslateTransform((bounds.left + bounds.right) * 0.5f, (bounds.top + bounds.bottom) * 0.5f);
    graphics.ScaleTransform(dpi / 96.0f, dpi / 96.0f);
    graphics.TranslateTransform(-8.0f, -8.0f);
    COLORREF ink = icon == UiIcon::Add ? GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT)
                                       : (disabled ? DisabledNavigationIconColor : NavigationIconColor);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(ink), GetGValue(ink), GetBValue(ink)), 1.6f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    switch (icon) {
    case UiIcon::More: {
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(ink), GetGValue(ink), GetBValue(ink)));
        for (float y : {3.25f, 8.0f, 12.75f})
            graphics.FillEllipse(&brush, 6.75f, y - 1.25f, 2.5f, 2.5f);
        break;
    }
    case UiIcon::ChevronRight: {
        pen.SetWidth(1.4f);
        const Gdiplus::PointF points[] = {{6.5f, 5.0f}, {9.5f, 8.0f}, {6.5f, 11.0f}};
        graphics.DrawLines(&pen, points, 3);
        break;
    }
    case UiIcon::Back:
    case UiIcon::Forward: {
        float sign = icon == UiIcon::Back ? -1.0f : 1.0f;
        graphics.DrawLine(&pen, 2.0f, 8.0f, 14.0f, 8.0f);
        graphics.DrawLine(&pen, 8.0f + sign * 6.0f, 8.0f, 8.0f + sign, 3.0f);
        graphics.DrawLine(&pen, 8.0f + sign * 6.0f, 8.0f, 8.0f + sign, 13.0f);
        break;
    }
    case UiIcon::Up: {
        graphics.DrawLine(&pen, 8.0f, 14.0f, 8.0f, 2.0f);
        graphics.DrawLine(&pen, 8.0f, 2.0f, 3.0f, 7.0f);
        graphics.DrawLine(&pen, 8.0f, 2.0f, 13.0f, 7.0f);
        break;
    }
    case UiIcon::Refresh: {
        const Gdiplus::PointF points0[] = {
            {12.583f, 6.000f},  {12.467f, 5.753f},  {12.337f, 5.512f},  {12.195f, 5.279f},
            {12.040f, 5.055f},  {11.874f, 4.838f},  {11.695f, 4.632f},  {11.506f, 4.435f},
            {11.306f, 4.249f},  {11.097f, 4.074f},  {10.878f, 3.911f},  {10.650f, 3.760f},
            {10.415f, 3.622f},  {10.172f, 3.497f},  {9.923f, 3.385f},   {9.669f, 3.287f},
            {9.409f, 3.203f},   {9.145f, 3.133f},   {8.878f, 3.078f},   {8.608f, 3.037f},
            {8.336f, 3.011f},   {8.063f, 3.000f},   {7.790f, 3.004f},   {7.518f, 3.023f},
            {7.247f, 3.057f},   {6.978f, 3.105f},   {6.713f, 3.169f},   {6.451f, 3.246f},
            {6.194f, 3.338f},   {5.942f, 3.443f},   {5.697f, 3.562f},   {5.458f, 3.695f},
            {5.227f, 3.840f},   {5.004f, 3.997f},   {4.790f, 4.167f},   {4.585f, 4.348f},
            {4.391f, 4.539f},   {4.208f, 4.742f},   {4.035f, 4.953f},   {3.875f, 5.174f},
            {3.727f, 5.404f},   {3.592f, 5.641f},   {3.469f, 5.885f},   {3.361f, 6.135f},
            {3.266f, 6.391f},   {3.185f, 6.652f},   {3.119f, 6.917f},   {3.067f, 7.185f},
            {3.030f, 7.455f},   {3.007f, 7.727f},   {3.000f, 8.000f},   {3.007f, 8.273f},
            {3.030f, 8.545f},   {3.067f, 8.815f},   {3.119f, 9.083f},   {3.185f, 9.348f},
            {3.266f, 9.609f},   {3.361f, 9.865f},   {3.469f, 10.115f},  {3.592f, 10.359f},
            {3.727f, 10.596f},  {3.875f, 10.826f},  {4.035f, 11.047f},  {4.208f, 11.258f},
            {4.391f, 11.461f},  {4.585f, 11.652f},  {4.790f, 11.833f},  {5.004f, 12.003f},
            {5.227f, 12.160f},  {5.458f, 12.305f},  {5.697f, 12.438f},  {5.942f, 12.557f},
            {6.194f, 12.662f},  {6.451f, 12.754f},  {6.713f, 12.831f},  {6.978f, 12.895f},
            {7.247f, 12.943f},  {7.518f, 12.977f},  {7.790f, 12.996f},  {8.063f, 13.000f},
            {8.336f, 12.989f},  {8.608f, 12.963f},  {8.878f, 12.922f},  {9.145f, 12.867f},
            {9.409f, 12.797f},  {9.669f, 12.713f},  {9.923f, 12.615f},  {10.172f, 12.503f},
            {10.415f, 12.378f}, {10.650f, 12.240f}, {10.878f, 12.089f}, {11.097f, 11.926f},
            {11.306f, 11.751f}, {11.506f, 11.565f}, {11.695f, 11.368f}, {11.874f, 11.162f},
            {12.040f, 10.945f}, {12.195f, 10.721f}, {12.337f, 10.488f}, {12.467f, 10.247f},
            {12.583f, 10.000f}};
        graphics.DrawLines(&pen, points0, 101);
        const Gdiplus::PointF points1[] = {{9.083f, 6.000f}, {12.583f, 6.000f}, {12.583f, 2.500f}};
        graphics.DrawLines(&pen, points1, 3);
        break;
    }
    case UiIcon::Add: {
        const Gdiplus::PointF points0[] = {{3.000f, 8.000f}, {13.000f, 8.000f}};
        graphics.DrawLines(&pen, points0, 2);
        const Gdiplus::PointF points1[] = {{8.000f, 3.000f}, {8.000f, 13.000f}};
        graphics.DrawLines(&pen, points1, 2);
        break;
    }
    }
}
} // namespace zen
