#include "tabs.h"
#include "ui_icons.h"
#include <set>
#include <gdiplus.h>
namespace zen {
TabStrip::TabStrip(App &a, int p) : app(a), pane(p) {}
TabStrip::~TabStrip() {
    if (ghost)
        DestroyWindow(ghost);
    if (IsWindow(hwnd)) {
        RevokeDragDrop(hwnd);
        DestroyWindow(hwnd);
    }
}
void TabStrip::Create() {
    WNDCLASSW wc{};
    wc.hInstance = app.instance;
    wc.lpszClassName = L"ZenTabs";
    wc.lpfnWndProc = Proc;
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                           app.window, nullptr, app.instance, this);
    tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                              WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                              CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, app.instance, nullptr);
    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = hwnd;
    tool.uId = (UINT_PTR)hwnd;
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, app.S(520));
    RegisterDragDrop(hwnd, this);
    SetTimer(hwnd, 1, 32, nullptr);
}
int TabStrip::Arrange(int width) {
    hits.clear();
    int row = app.S(32), x = 0, y = 0;
    HDC dc = GetDC(hwnd);
    auto old = SelectObject(dc, app.font);
    auto add = [&](TabHit h, const std::wstring &text, int extra) {
        // 源标签从预览流中移除，余下标签自然补位；Model 仍保持原顺序。
        for (auto &strip : app.strips) {
            if (!strip || !strip->dragging || strip->pane != pane)
                continue;
            if (strip->source.tab) {
                if (h.tab == strip->source.tab)
                    return;
                if (!h.tab && h.group && h.group == strip->source.group &&
                    std::count_if(app.model.panes[pane].tabs.begin(), app.model.panes[pane].tabs.end(),
                                  [&](const Tab &t) { return t.group == h.group; }) == 1)
                    return;
            } else if (h.group && h.group == strip->source.group)
                return;
        }
        SIZE s{};
        GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &s);
        int w = h.plus              ? app.S(32)
                : !h.tab && h.group ? std::max(app.S(32), (int)s.cx + app.S(24))
                                    : std::clamp((int)s.cx + app.S(extra), app.S(58), app.S(210));
        w = std::min(w, std::max(1, width));
        if (app.model.wrap && x && x + w > width) {
            x = 0;
            y += row;
        }
        h.rect = {x, y, x + w, y + row};
        hits.push_back(h);
        x += w;
    };
    Id last = 0;
    for (auto &t : app.model.panes[pane].tabs) {
        if (t.group != last) {
            last = t.group;
            if (last) {
                auto *g = app.model.FindGroup(pane, last);
                if (g)
                    add({{}, 0, last, false}, g->name, 28);
            }
        }
        auto *g = t.group ? app.model.FindGroup(pane, t.group) : nullptr;
        if (g && g->collapsed)
            continue;
        add({{}, t.id, t.group, false}, app.Title(t), 47);
    }
    add({{}, 0, 0, true}, L"+", 0);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    total = app.model.wrap ? y + row : x;
    height = app.model.wrap ? std::min(total, row * 3) : row;
    scroll = std::clamp(scroll, 0, std::max(0, total - (app.model.wrap ? height : width)));
    for (auto &h : hits)
        OffsetRect(&h.rect, app.model.wrap ? 0 : -scroll, app.model.wrap ? -scroll : 0);
    naturalHits = hits;
    SetRectEmpty(&gap);
    // 预览只调整绘制位置；真实标签顺序直到释放鼠标时才提交。
    for (auto &strip : app.strips) {
        if (!strip || !strip->dragging || strip->dropPane != pane)
            continue;
        int px = 0, py = 0;
        bool inserted = false;
        auto place = [&](int w) {
            if (app.model.wrap && px && px + w > width) {
                px = 0;
                py += row;
            }
            RECT r{px, py, px + w, py + row};
            px += w;
            return r;
        };
        int gapWidth = std::min(width, (int)(strip->source.rect.right - strip->source.rect.left));
        for (auto &h : hits) {
            bool at = h.plus || (h.tab && app.model.Index(pane, h.tab) >= strip->dropIndex);
            if (!inserted && at) {
                gap = place(gapWidth);
                inserted = true;
            }
            int w = h.rect.right - h.rect.left;
            h.rect = place(w);
        }
        total = app.model.wrap ? py + row : px;
        height = app.model.wrap ? std::min(total, row * 3) : row;
        scroll = std::clamp(scroll, 0, std::max(0, total - (app.model.wrap ? height : width)));
        for (auto &h : hits)
            OffsetRect(&h.rect, app.model.wrap ? 0 : -scroll, app.model.wrap ? -scroll : 0);
        OffsetRect(&gap, app.model.wrap ? 0 : -scroll, app.model.wrap ? -scroll : 0);
    }
    return height;
}
TabHit TabStrip::Hit(POINT p) const {
    for (auto &h : hits)
        if (PtInRect(&h.rect, p))
            return h;
    return {};
}
void TabStrip::EnsureVisible(Id id) {
    auto *t = app.model.Find(id);
    if (!t)
        return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    for (auto &h : hits)
        if (h.tab == id || (!h.tab && h.group == t->group && app.model.FindGroup(pane, h.group)->collapsed)) {
            int a = app.model.wrap ? h.rect.top : h.rect.left;
            int b = app.model.wrap ? h.rect.bottom : h.rect.right;
            int limit = app.model.wrap ? rc.bottom : rc.right;
            if (a < 0)
                scroll += a;
            else if (b > limit)
                scroll += b - limit;
            break;
        }
    app.Layout();
}
void TabStrip::Paint() {
    PAINTSTRUCT ps;
    HDC out = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(out);
    HBITMAP bmp = CreateCompatibleBitmap(out, std::max(1L, rc.right), std::max(1L, rc.bottom));
    auto oldBmp = SelectObject(dc, bmp);
    FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
    auto oldFont = SelectObject(dc, app.font);
    SetBkMode(dc, TRANSPARENT);
    for (auto &h : hits) {
        if (h.rect.bottom < 0 || h.rect.top >= rc.bottom || h.rect.right < 0 || h.rect.left >= rc.right)
            continue;
        bool selected = h.tab == app.model.panes[pane].selected;
        auto *g = h.group ? app.model.FindGroup(pane, h.group) : nullptr;
        bool over = PtInRect(&h.rect, mouse);
        RECT r = h.rect;
        InflateRect(&r, -app.S(2), -app.S(2));
        COLORREF color = selected ? GetSysColor(COLOR_WINDOW)
                         : over   ? RGB(228, 232, 238)
                                  : GetSysColor(COLOR_BTNFACE);
        bool placeholder = false;
        for (auto &strip : app.strips)
            if (strip && strip->dragging && strip->pane == pane)
                placeholder |=
                    strip->source.tab ? h.tab == strip->source.tab : h.group == strip->source.group;
        if (placeholder)
            color = GetSysColor(COLOR_BTNFACE);
        HBRUSH bg = CreateSolidBrush(color);
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        if (placeholder)
            continue;
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        if (!h.plus) {
            r.left += app.S(8);
            r.right -= app.S(8);
        }
        if (h.plus) {
            DrawUiIcon(dc, r, UiIcon::Add, app.dpi);
        } else if (h.tab) {
            auto *t = app.model.Find(h.tab);
            if (!t)
                continue;
            HICON icon = app.Icon(h.tab);
            if (icon)
                DrawIconEx(dc, r.left, r.top + app.S(5), icon, app.S(16), app.S(16), 0, nullptr, DI_NORMAL);
            if (!t->name.empty()) {
                RECT star{r.left + app.S(16) - app.S(12), r.top + app.S(5) + app.S(16) - app.S(12),
                          r.left + app.S(16), r.top + app.S(5) + app.S(16)};
                DrawFavoriteBadge(dc, star);
            }
            r.left += app.S(23);
            auto text = app.Title(*t);
            DrawTextW(dc, text.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (over) {
                RECT cross = CloseRect(h);
                // 先覆盖文字，后绘制按钮；不调整文本矩形或标签宽度。
                // 渐变末端在按钮左缘达到全不透明，按钮下方不残留文字。
                int fadeLeft = std::max((int)r.left, (int)cross.left - app.S(6));
                Gdiplus::Graphics graphics(dc);
                Gdiplus::Color transparent(0, GetRValue(color), GetGValue(color), GetBValue(color));
                Gdiplus::Color opaque(255, GetRValue(color), GetGValue(color), GetBValue(color));
                if (cross.left > fadeLeft) {
                    Gdiplus::LinearGradientBrush fade(Gdiplus::Point(fadeLeft, 0),
                                                      Gdiplus::Point(cross.left, 0), transparent, opaque);
                    graphics.FillRectangle(&fade, fadeLeft, h.rect.top + app.S(3), cross.left - fadeLeft,
                                           h.rect.bottom - h.rect.top - app.S(7));
                }
                Gdiplus::SolidBrush cover(opaque);
                graphics.FillRectangle(&cover, Gdiplus::Rect(cross.left, h.rect.top + app.S(3),
                                                             h.rect.right - app.S(2) - cross.left,
                                                             h.rect.bottom - h.rect.top - app.S(7)));
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                if (PtInRect(&cross, mouse)) {
                    Gdiplus::SolidBrush hover(Gdiplus::Color(255, 208, 214, 222));
                    graphics.FillEllipse(&hover,
                                         Gdiplus::Rect(cross.left, cross.top, cross.right - cross.left,
                                                       cross.bottom - cross.top));
                }
                float cx = (cross.left + cross.right) * 0.5f, cy = (cross.top + cross.bottom) * 0.5f;
                float arm = app.dpi / 96.0f * 3.0f;
                COLORREF ink = GetSysColor(COLOR_WINDOWTEXT);
                Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(ink), GetGValue(ink), GetBValue(ink)),
                                 app.dpi / 96.0f * 1.4f);
                pen.SetStartCap(Gdiplus::LineCapRound);
                pen.SetEndCap(Gdiplus::LineCapRound);
                graphics.DrawLine(&pen, cx - arm, cy - arm, cx + arm, cy + arm);
                graphics.DrawLine(&pen, cx - arm, cy + arm, cx + arm, cy - arm);
            }
        } else if (g) {
            RECT chip = h.rect;
            InflateRect(&chip, -app.S(3), -app.S(5));
            {
                Gdiplus::Graphics graphics(dc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::GraphicsPath path;
                float x = (float)chip.left, y = (float)chip.top, w = (float)(chip.right - chip.left),
                      hh = (float)(chip.bottom - chip.top);
                float diameter = (float)std::min(app.S(8), std::min((int)w, (int)hh));
                if (diameter > 0) {
                    path.AddArc(x, y, diameter, diameter, 180, 90);
                    path.AddArc(x + w - diameter, y, diameter, diameter, 270, 90);
                    path.AddArc(x + w - diameter, y + hh - diameter, diameter, diameter, 0, 90);
                    path.AddArc(x, y + hh - diameter, diameter, diameter, 90, 90);
                    path.CloseFigure();
                    Gdiplus::SolidBrush brush(
                        Gdiplus::Color(255, GetRValue(g->color), GetGValue(g->color), GetBValue(g->color)));
                    graphics.FillPath(&brush, &path);
                }
            }
            InflateRect(&chip, -app.S(9), 0);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, g->name.c_str(), -1, &chip,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    // 底线最后绘制并覆盖元素间距：展开组同一行连续，换行后自然分段。
    for (const auto &h : hits) {
        auto *group = h.group ? app.model.FindGroup(pane, h.group) : nullptr;
        if (!group || group->collapsed)
            continue;
        RECT line{h.rect.left, h.rect.bottom - app.S(3), h.rect.right, h.rect.bottom};
        auto brush = CreateSolidBrush(group->color);
        FillRect(dc, &line, brush);
        DeleteObject(brush);
    }
    // 仅在同一行相邻的两个普通状态标签之间绘制分隔线。
    for (size_t i = 1; i < hits.size(); ++i) {
        auto &left = hits[i - 1];
        auto &right = hits[i];
        if (!left.tab || (!right.tab && !right.plus) || left.rect.top != right.rect.top)
            continue;
        if (left.tab == app.model.panes[pane].selected || right.tab == app.model.panes[pane].selected)
            continue;
        auto pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_GRAYTEXT));
        auto old = SelectObject(dc, pen);
        int x = right.rect.left, y = (right.rect.top + right.rect.bottom) / 2;
        MoveToEx(dc, x, y - app.S(7), nullptr);
        LineTo(dc, x, y + app.S(7));
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    for (auto &strip : app.strips)
        if (strip && strip->dragging && strip->dropPane == pane) {
            auto *g = app.model.FindGroup(pane, strip->dropGroup);
            auto br = CreateSolidBrush(g ? g->color : RGB(55, 125, 220));
            FrameRect(dc, &rc, br);
            if (!IsRectEmpty(&gap)) {
                RECT marker = gap;
                marker.right = marker.left + app.S(3);
                FillRect(dc, &marker, br);
                FrameRect(dc, &gap, br);
            }
            DeleteObject(br);
        }
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}
void TabStrip::RestoreExpanded() {
    if (!expanded.empty())
        app.Changed();
    for (const auto &state : expanded)
        app.model.RestoreGroups(state);
    expanded.clear();
    app.SyncViews();
}
void TabStrip::FinishExternal(bool success) {
    external = false;
    externalData.Reset();
    releasedAt = 0;
    if (success) {
        expanded.clear();
        app.Changed();
    } else
        RestoreExpanded();
}
std::vector<GroupExpansionState> TabStrip::DetachExternal() {
    external = false;
    externalData.Reset();
    releasedAt = 0;
    auto result = std::move(expanded);
    expanded.clear();
    return result;
}
void TabStrip::Hover(POINT screen, bool files) {
    for (int p = 0; p < 2; ++p) {
        auto &strip = *app.strips[p];
        RECT wr;
        GetWindowRect(strip.hwnd, &wr);
        if (!PtInRect(&wr, screen))
            continue;
        POINT local = screen;
        ScreenToClient(strip.hwnd, &local);
        auto hit = strip.Hit(local);
        Id target = hit.tab ? hit.tab : hit.group;
        Id prev = hoverTab ? hoverTab : hoverGroup;
        if (prev != target) {
            hoverTab = hit.tab;
            hoverGroup = hit.tab ? 0 : hit.group;
            hoverTime = GetTickCount64();
        }
        if (GetTickCount64() - hoverTime > 550) {
            if (hoverGroup) {
                auto *g = app.model.FindGroup(p, hoverGroup);
                if (g && g->collapsed) {
                    // 文件跨栏经过多个接收者时，同一栏只保留第一次展开前的快照。
                    auto &snapshots = files ? strip.expanded : expanded;
                    if (std::none_of(snapshots.begin(), snapshots.end(),
                                     [&](const auto &state) { return state.pane == p; }))
                        snapshots.push_back(app.model.CaptureGroups(p));
                    app.model.SetGroupCollapsed(p, hoverGroup, false);
                    app.Changed();
                    app.SyncViews();
                }
            } else if (files && hoverTab && app.model.panes[p].selected != hoverTab)
                app.Activate(p, hoverTab);
        }
        int oldScroll = strip.scroll;
        int extent = app.model.wrap ? wr.bottom - wr.top : wr.right - wr.left;
        int pos = app.model.wrap ? local.y : local.x;
        if (pos < app.S(16))
            strip.scroll = std::max(0, strip.scroll - app.S(8));
        else if (pos > extent - app.S(16))
            strip.scroll += app.S(8);
        if (oldScroll != strip.scroll)
            app.Layout(false);
        break;
    }
}
void TabStrip::ResolveDrop(POINT screen, int &target, int &index, Id &group) {
    target = -1;
    index = 0;
    group = 0;
    for (int p = 0; p < 2; ++p) {
        RECT wr;
        GetWindowRect(app.strips[p]->hwnd, &wr);
        if (!PtInRect(&wr, screen))
            continue;
        target = p;
        POINT local = screen;
        ScreenToClient(app.strips[p]->hwnd, &local);
        TabHit hit{};
        for (auto &h : app.strips[p]->naturalHits)
            if (PtInRect(&h.rect, local)) {
                hit = h;
                break;
            }
        auto &v = app.model.panes[p].tabs;
        index = (int)v.size();
        if (hit.tab) {
            index = app.model.Index(p, hit.tab) + (local.x > (hit.rect.left + hit.rect.right) / 2);
            group = hit.group;
        } else if (hit.group) {
            group = hit.group;
            index = 0;
            for (int i = 0; i < (int)v.size(); ++i)
                if (v[i].group == group)
                    index = i + 1;
        }
        break;
    }
    if (target >= 0 && !source.tab && group) {
        for (int i = 0; i < (int)app.model.panes[target].tabs.size(); ++i)
            if (app.model.panes[target].tabs[i].group == group)
                index = i + 1;
    }
}
RECT TabStrip::CloseRect(const TabHit &h) const {
    int cy = (h.rect.top + h.rect.bottom) / 2;
    return {h.rect.right - app.S(23), cy - app.S(9), h.rect.right - app.S(5), cy + app.S(9)};
}

void TabStrip::UpdateDrag(POINT screen) {
    int oldPane = dropPane, oldIndex = dropIndex;
    Id oldGroup = dropGroup;
    ResolveDrop(screen, dropPane, dropIndex, dropGroup);
    SetCursor(LoadCursor(nullptr, dropPane < 0 ? IDC_NO : IDC_SIZEALL));
    if (!ghost) {
        std::wstring text;
        if (auto *t = app.model.Find(source.tab))
            text = app.Title(*t);
        else if (auto *g = app.model.FindGroup(pane, source.group))
            text = g->name;
        ghost = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                L"STATIC", text.c_str(), WS_POPUP | WS_BORDER | SS_CENTER | SS_CENTERIMAGE, 0,
                                0, 1, 1, app.window, nullptr, app.instance, nullptr);
        SendMessageW(ghost, WM_SETFONT, (WPARAM)app.font, FALSE);
        SetLayeredWindowAttributes(ghost, 0, 220, LWA_ALPHA);
    }
    SetWindowPos(ghost, HWND_TOPMOST, screen.x - (down.x - source.rect.left),
                 screen.y - (down.y - source.rect.top), source.rect.right - source.rect.left,
                 source.rect.bottom - source.rect.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (oldPane != dropPane || oldIndex != dropIndex || oldGroup != dropGroup)
        app.Layout(false);
}
void TabStrip::EndDrag() {
    dragging = false;
    dropPane = -1;
    if (ghost) {
        DestroyWindow(ghost);
        ghost = nullptr;
    }
    SendMessageW(tooltip, TTM_ACTIVATE, TRUE, 0);
    app.Layout();
}
void TabStrip::FinishMove(POINT screen) {
    int target = -1, index = 0;
    Id group = 0;
    ResolveDrop(screen, target, index, group);
    if (target < 0) {
        RestoreExpanded();
        return;
    }
    // 先恢复悬停改变的状态，避免临时补选影响“整组是否携带当前标签”的判断。
    bool keepExpanded = false;
    if (source.tab && group)
        for (const auto &state : expanded)
            if (state.pane == target)
                for (const auto &[id, collapsed] : state.groups)
                    if (id == group && collapsed)
                        if (auto *g = app.model.FindGroup(target, group))
                            keepExpanded = !g->collapsed;
    RestoreExpanded();
    if (keepExpanded)
        app.model.SetGroupCollapsed(target, group, false);
    if (!source.tab && source.group) {
        if (group == source.group && target == pane) {
            RestoreExpanded();
            return;
        }
        if (group) {
            auto &v = app.model.panes[target].tabs;
            index = 0;
            for (int i = 0; i < (int)v.size(); ++i)
                if (v[i].group == group)
                    index = i + 1;
        }
        app.model.MoveGroup(pane, source.group, target, index);
    } else if (source.tab) {
        app.model.Move(source.tab, target, index, group);
    }
    app.SyncViews();
    app.Changed();
    app.Activate(app.model.active);
}
LRESULT CALLBACK TabStrip::Proc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto *s = (TabStrip *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        s = (TabStrip *)((CREATESTRUCTW *)b)->lpCreateParams;
        s->hwnd = w;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)s);
    }
    return s ? s->Message(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT TabStrip::Message(UINT m, WPARAM a, LPARAM b) {
    POINT pt{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
    switch (m) {
    case WM_NOTIFY:
        if (((NMHDR *)b)->code == TTN_GETDISPINFOW) {
            POINT cursor;
            GetCursorPos(&cursor);
            ScreenToClient(hwnd, &cursor);
            auto hit = Hit(cursor);
            tipText.clear();
            if (auto *t = app.model.Find(hit.tab)) {
                tipText = app.Title(*t) + L"\n" + t->path;
                if (!t->name.empty())
                    tipText += L"\n主目录：" + t->home;
            } else if (auto *g = app.model.FindGroup(pane, hit.group))
                tipText = g->name;
            ((NMTTDISPINFOW *)b)->lpszText = tipText.data();
            return 0;
        }
        break;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        source = Hit(pt);
        down = pt;
        {
            auto r = CloseRect(source);
            closePressed = source.tab && PtInRect(&r, pt);
        }
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        mouse = pt;
        TRACKMOUSEEVENT tr{sizeof(tr), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tr);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (GetCapture() == hwnd && (source.tab || source.group) && !dragging && !closePressed &&
            (abs(pt.x - down.x) > GetSystemMetrics(SM_CXDRAG) ||
             abs(pt.y - down.y) > GetSystemMetrics(SM_CYDRAG))) {
            dragging = true;
            SendMessageW(tooltip, TTM_ACTIVATE, FALSE, 0);
        }
        if (dragging) {
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            UpdateDrag(screen);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        mouse = {-1, -1};
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (dragging || external) {
            POINT screen;
            GetCursorPos(&screen);
            Hover(screen, external);
            if (dragging)
                UpdateDrag(screen);
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                EndDrag();
                source = {};
                external = false;
                ReleaseCapture();
                RestoreExpanded();
            } else if (external &&
                       !((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)) {
                if (!releasedAt)
                    releasedAt = GetTickCount64();
                if (GetTickCount64() - releasedAt >= 200) {
                    bool success = false;
                    for (const auto *format : {CFSTR_PERFORMEDDROPEFFECT, CFSTR_LOGICALPERFORMEDDROPEFFECT}) {
                        FORMATETC f{(CLIPFORMAT)RegisterClipboardFormatW(format), nullptr, DVASPECT_CONTENT,
                                    -1, TYMED_HGLOBAL};
                        STGMEDIUM medium{};
                        if (externalData && SUCCEEDED(externalData->GetData(&f, &medium))) {
                            if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal &&
                                GlobalSize(medium.hGlobal) >= sizeof(DWORD))
                                if (auto *value = (DWORD *)GlobalLock(medium.hGlobal)) {
                                    success |= *value != DROPEFFECT_NONE;
                                    GlobalUnlock(medium.hGlobal);
                                }
                            ReleaseStgMedium(&medium);
                        }
                    }
                    FinishExternal(success);
                }
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        bool was = dragging;
        dragging = false;
        ReleaseCapture();
        if (was) {
            POINT s = pt;
            ClientToScreen(hwnd, &s);
            FinishMove(s);
            EndDrag();
            source = {};
            return 0;
        }
        auto hit = Hit(pt);
        if (hit.tab != source.tab || hit.group != source.group || hit.plus != source.plus)
            return 0;
        if (hit.plus) {
            app.NewTab(pane);
            return 0;
        }
        if (hit.tab) {
            auto close = CloseRect(hit);
            if (closePressed && PtInRect(&close, pt))
                app.CloseTabs(pane, {hit.tab});
            else
                app.Activate(pane, hit.tab, true);
        } else if (hit.group) {
            auto *g = app.model.FindGroup(pane, hit.group);
            if (g) {
                app.model.SetGroupCollapsed(pane, hit.group, !g->collapsed);
                app.Changed();
                app.SyncViews();
                app.strips[pane]->EnsureVisible(app.model.panes[pane].selected);
            }
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        auto hit = Hit(pt);
        if (auto *t = app.model.Find(hit.tab); t && !t->name.empty())
            app.Navigate(hit.tab, t->home);
        return 0;
    }
    case WM_RBUTTONUP: {
        auto hit = Hit(pt);
        POINT s = pt;
        ClientToScreen(hwnd, &s);
        if (hit.tab || hit.group)
            app.TabMenu(pane, hit.tab, hit.tab ? 0 : hit.group, s);
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll -= GET_WHEEL_DELTA_WPARAM(a) / WHEEL_DELTA * app.S(32);
        scroll = std::max(0, scroll);
        app.Layout();
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging) {
            EndDrag();
            source = {};
            RestoreExpanded();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, m, a, b);
}
HRESULT TabStrip::QueryInterface(REFIID iid, void **p) {
    if (!p)
        return E_POINTER;
    *p = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDropTarget) {
        *p = static_cast<IDropTarget *>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}
HRESULT TabStrip::DragEnter(IDataObject *data, DWORD, POINTL p, DWORD *e) {
    externalData = data;
    releasedAt = 0;
    external = true;
    hoverTime = GetTickCount64();
    return DragOver(0, p, e);
}
HRESULT TabStrip::DragOver(DWORD, POINTL p, DWORD *e) {
    Hover({p.x, p.y}, true);
    *e = DROPEFFECT_NONE;
    return S_OK;
}
HRESULT TabStrip::DragLeave() {
    return S_OK;
}
HRESULT TabStrip::Drop(IDataObject *, DWORD, POINTL, DWORD *e) {
    *e = DROPEFFECT_NONE;
    FinishExternal(false);
    return S_OK;
}
} // namespace zen
