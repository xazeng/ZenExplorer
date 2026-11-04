#include "model.h"
#include <set>
#include <cmath>
namespace zen {
Model::Model() {
    Add(0);
    Add(1);
    active = 0;
}
Tab *Model::Find(Id id) {
    for (auto &p : panes)
        for (auto &t : p.tabs)
            if (t.id == id)
                return &t;
    return nullptr;
}
const Tab *Model::Find(Id id) const {
    return const_cast<Model *>(this)->Find(id);
}
Group *Model::FindGroup(int p, Id id) {
    for (auto &g : panes[p].groups)
        if (g.id == id)
            return &g;
    return nullptr;
}
int Model::PaneOf(Id id) const {
    for (int p = 0; p < 2; ++p)
        for (auto &t : panes[p].tabs)
            if (t.id == id)
                return p;
    return -1;
}
int Model::Index(int p, Id id) const {
    auto &v = panes[p].tabs;
    for (int i = 0; i < (int)v.size(); ++i)
        if (v[i].id == id)
            return i;
    return -1;
}
Id Model::Add(int p, const std::wstring &path, Id group, int index, bool activate, Id opener) {
    if (p < 0 || p > 1 || path.empty())
        return 0;
    if (group && !FindGroup(p, group))
        return 0;
    Tab t;
    t.id = nextId++;
    t.group = group;
    t.opener = PaneOf(opener) == p ? opener : 0;
    t.path = path;
    t.history = {path};
    auto id = t.id;
    auto &v = panes[p].tabs;
    if (index < 0 || index > (int)v.size())
        index = (int)v.size();
    v.insert(v.begin() + index, std::move(t));
    if (activate && group)
        SetGroupCollapsed(p, group, false);
    if (activate || !panes[p].selected) {
        panes[p].selected = id;
        if (activate)
            active = p;
    }
    return id;
}
void Model::Navigate(Id id, const std::wstring &path, bool append) {
    auto *t = Find(id);
    if (!t || path.empty())
        return;
    if (t->path != path) {
        t->displayName.clear();
        t->viewMode = FVM_DETAILS;
        t->iconSize = 16;
    }
    t->path = path;
    if (append && (t->history.empty() || t->history[t->historyIndex] != path)) {
        t->history.resize(t->historyIndex + 1);
        t->history.push_back(path);
        if (t->history.size() > 128)
            t->history.erase(t->history.begin());
        t->historyIndex = (int)t->history.size() - 1;
    }
}
void Model::Rename(Id id, const std::wstring &n) {
    if (auto *t = Find(id)) {
        if (n.empty()) {
            t->name.clear();
            t->home.clear();
        } else {
            if (t->name.empty())
                t->home = t->path;
            t->name = n;
        }
    }
}
void Model::Record(const std::vector<std::wstring> &paths) {
    std::vector<std::wstring> out;
    auto add = [&](const std::wstring &s) {
        if (!s.empty() && std::none_of(out.begin(), out.end(), [&](auto &x) {
                return CompareStringOrdinal(s.c_str(), -1, x.c_str(), -1, TRUE) == CSTR_EQUAL;
            }))
            out.push_back(s);
    };
    for (auto &s : paths)
        add(s);
    for (auto &s : recent)
        add(s);
    if (out.size() > 10)
        out.resize(10);
    recent = std::move(out);
}
void Model::Cleanup(int p) {
    auto &pane = panes[p];
    std::erase_if(pane.groups, [&](auto &g) {
        return std::none_of(pane.tabs.begin(), pane.tabs.end(), [&](auto &t) { return t.group == g.id; });
    });
    for (auto &g : pane.groups)
        if (auto *t = Find(g.lastSelected); !t || t->group != g.id || PaneOf(t->id) != p)
            g.lastSelected = 0;
    if (pane.tabs.empty())
        Add(p, Home, 0, -1, false);
    if (Index(p, pane.selected) < 0)
        pane.selected = pane.tabs.front().id;
    EnsureVisibleSelection(p);
}
bool Model::Visible(int p, Id id) const {
    if (p < 0 || p > 1 || PaneOf(id) != p)
        return false;
    auto *tab = Find(id);
    for (const auto &g : panes[p].groups)
        if (g.id == tab->group)
            return !g.collapsed;
    return true;
}
void Model::EnsureVisibleSelection(int p) {
    if (p < 0 || p > 1 || Visible(p, panes[p].selected))
        return;
    auto &tabs = panes[p].tabs;
    int index = Index(p, panes[p].selected);
    Id replacement = 0;
    for (int i = index + 1; i < (int)tabs.size(); ++i)
        if (Visible(p, tabs[i].id)) {
            replacement = tabs[i].id;
            break;
        }
    if (!replacement)
        for (int i = index - 1; i >= 0; --i)
            if (Visible(p, tabs[i].id)) {
                replacement = tabs[i].id;
                break;
            }
    if (!replacement)
        replacement = Add(p, Home, 0, -1, false);
    panes[p].selected = replacement;
    Log(L"隐藏标签补选 pane=" + std::to_wstring(p) + L" tab=" + std::to_wstring(replacement));
}
void Model::Remove(int p, const std::vector<Id> &ids, bool record, bool wholeGroup) {
    if (p < 0 || p > 1)
        return;
    auto &v = panes[p].tabs;
    auto removed = [&](Id id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    int sel = Index(p, panes[p].selected);
    Id next = panes[p].selected;
    std::vector<std::wstring> paths;
    if (sel >= 0 && removed(next)) {
        next = 0;
        Id group = v[sel].group;
        auto eligible = [&](const Tab &t, bool same) {
            return !removed(t.id) && (!same || t.group == group) && Visible(p, t.id);
        };
        auto scan = [&](bool same) {
            for (int i = sel + 1; i < (int)v.size(); ++i)
                if (eligible(v[i], same))
                    return v[i].id;
            for (int i = sel - 1; i >= 0; --i)
                if (eligible(v[i], same))
                    return v[i].id;
            return Id(0);
        };
        Id opener = v[sel].opener;
        if (record && !wholeGroup && ids.size() == 1 && !removed(opener) && Visible(p, opener))
            next = opener;
        if (!next && group)
            next = scan(true);
        if (!next)
            next = scan(false);
    }
    for (auto &t : v)
        if (removed(t.id))
            paths.push_back(t.path);
    if (record)
        Record(paths);
    std::erase_if(v, [&](auto &t) { return removed(t.id); });
    panes[p].selected = next;
    if (!next)
        Add(p, Home, 0, -1, false);
    Cleanup(p);
}
Id Model::MakeGroup(int p, Id id) {
    if (p < 0 || p > 1 || PaneOf(id) != p)
        return 0;
    Group g;
    g.id = nextId++;
    size_t least = panes[p].groups.size() + 1;
    for (const auto &preset : GroupColors) {
        size_t count = std::count_if(panes[p].groups.begin(), panes[p].groups.end(),
                                     [&](const auto &x) { return x.color == preset.color; });
        if (count < least) {
            least = count;
            g.color = preset.color;
        }
    }
    auto gid = g.id;
    // 先将旧组中的标签移出，防止把连续组拆成两段。
    AssignGroup(id, 0);
    panes[p].groups.push_back(g);
    Find(id)->group = gid;
    Find(id)->opener = 0;
    SetGroupCollapsed(p, gid, false);
    return gid;
}
void Model::AssignGroup(Id id, Id group) {
    int p = PaneOf(id);
    if (p < 0 || (group && !FindGroup(p, group)))
        return;
    auto &v = panes[p].tabs;
    int old = Index(p, id);
    Id oldGroup = v[old].group;
    if (oldGroup == group)
        return;
    Tab t = std::move(v[old]);
    t.opener = 0;
    v.erase(v.begin() + old);
    int dest = old;
    if (group) {
        dest = 0;
        for (int i = 0; i < (int)v.size(); ++i)
            if (v[i].group == group)
                dest = i + 1;
    } else if (oldGroup) {
        for (int i = 0; i < (int)v.size(); ++i)
            if (v[i].group == oldGroup)
                dest = i + 1;
    }
    t.group = group;
    v.insert(v.begin() + std::min(dest, (int)v.size()), std::move(t));
    Cleanup(p);
}
void Model::SetGroupCollapsed(int p, Id group, bool collapsed) {
    if (p < 0 || p > 1)
        return;
    auto *g = FindGroup(p, group);
    if (!g)
        return;
    // 必须在改变本栏选择前记录被收起组的当前标签。
    auto remember = [&](Group &item) {
        if (!item.collapsed)
            if (auto *tab = Find(panes[p].selected); tab && tab->group == item.id)
                item.lastSelected = tab->id;
    };
    if (collapsed)
        remember(*g);
    if (!collapsed && singleExpandedGroup) {
        for (auto &other : panes[p].groups)
            if (other.id != group) {
                remember(other);
                other.collapsed = true;
            }
        // 目标组仍保持原状态，先完成收起其他组后的补选，不能借新展开的标签补选。
        EnsureVisibleSelection(p);
    }
    g->collapsed = collapsed;
    // 展开仅改变可见性；只有原选中项被自动折叠隐藏时才按通用规则补选。
    EnsureVisibleSelection(p);
    Log(L"分组展开状态 pane=" + std::to_wstring(p) + L" group=" + std::to_wstring(group) + L" collapsed=" +
        std::to_wstring(collapsed));
}
void Model::NormalizeGroups(int p) {
    if (p < 0 || p > 1)
        return;
    if (singleExpandedGroup) {
        Id keep = 0;
        if (auto *tab = Find(panes[p].selected); tab && PaneOf(tab->id) == p)
            keep = tab->group;
        if (!keep)
            for (const auto &tab : panes[p].tabs)
                if (auto *g = FindGroup(p, tab.group); g && !g->collapsed) {
                    keep = g->id;
                    break;
                }
        for (auto &g : panes[p].groups) {
            if (g.id != keep && !g.collapsed)
                if (auto *tab = Find(panes[p].selected); tab && tab->group == g.id)
                    g.lastSelected = tab->id;
            g.collapsed = g.id != keep;
        }
    }
    EnsureVisibleSelection(p);
}
void Model::SetSingleExpandedGroup(bool enabled) {
    singleExpandedGroup = enabled;
    for (int p = 0; p < 2; ++p)
        NormalizeGroups(p);
    Log(L"每栏仅展开一个组 enabled=" + std::to_wstring(enabled));
}
GroupExpansionState Model::CaptureGroups(int p) const {
    GroupExpansionState state;
    if (p < 0 || p > 1)
        return state;
    state.pane = p;
    state.selected = panes[p].selected;
    for (const auto &g : panes[p].groups)
        state.groups.push_back({g.id, g.collapsed, g.lastSelected});
    return state;
}
void Model::RestoreGroups(const GroupExpansionState &state) {
    if (state.pane < 0 || state.pane > 1)
        return;
    for (const auto &[id, collapsed, lastSelected] : state.groups)
        if (auto *g = FindGroup(state.pane, id)) {
            g->collapsed = collapsed;
            g->lastSelected = lastSelected;
        }
    if (Visible(state.pane, state.selected))
        panes[state.pane].selected = state.selected;
    NormalizeGroups(state.pane);
}
void Model::Ungroup(int p, Id group) {
    for (auto &t : panes[p].tabs)
        if (t.group == group) {
            t.group = 0;
            t.opener = 0;
        }
    Cleanup(p);
}
void Model::Move(Id id, int target, int index, Id group) {
    int source = PaneOf(id);
    if (source < 0 || target < 0 || target > 1 || (group && !FindGroup(target, group)))
        return;
    int from = Index(source, id);
    Tab t = *Find(id);
    if (source != target || t.group != group)
        t.opener = 0;
    auto &dest = panes[target].tabs;
    index = std::clamp(index, 0, (int)dest.size());
    if (source == target && from < index)
        --index;
    // 同栏删除不提前清理目标组；选中标签的补选复用关闭规则。
    if (source == target) {
        auto &v = panes[source].tabs;
        v.erase(v.begin() + from);
        t.group = group;
        v.insert(v.begin() + std::clamp(index, 0, (int)v.size()), std::move(t));
        panes[target].selected = id;
        Cleanup(target);
    } else {
        Remove(source, {id}, false);
        t.group = group;
        dest.insert(dest.begin() + std::clamp(index, 0, (int)dest.size()), std::move(t));
        panes[target].selected = id;
        Cleanup(target);
    }
    active = target;
}
void Model::MoveGroup(int source, Id group, int target, int index) {
    if (source < 0 || source > 1 || target < 0 || target > 1)
        return;
    auto *g = FindGroup(source, group);
    if (!g)
        return;
    Group copy = *g;
    std::vector<Tab> moving;
    std::vector<Id> ids;
    Id selected = panes[source].selected;
    int oldActive = active;
    bool carries = false;
    int before = 0;
    for (int i = 0; i < (int)panes[source].tabs.size(); ++i) {
        auto &t = panes[source].tabs[i];
        if (t.group == group) {
            moving.push_back(t);
            if (source != target)
                moving.back().opener = 0;
            ids.push_back(t.id);
            carries |= t.id == selected;
            if (source == target && i < index)
                ++before;
        }
    }
    if (source == target) {
        std::erase_if(panes[source].tabs, [&](auto &t) { return t.group == group; });
        index -= before;
    } else {
        Remove(source, ids, false);
        panes[target].groups.push_back(copy);
    }
    auto &v = panes[target].tabs;
    index = std::clamp(index, 0, (int)v.size());
    v.insert(v.begin() + index, moving.begin(), moving.end());
    if (carries) {
        SetGroupCollapsed(target, group, false);
        panes[target].selected = selected;
        active = target;
    } else
        active = oldActive;
    if (source != target && !carries)
        NormalizeGroups(target);
    Cleanup(target);
}
bool Model::Valid() const {
    if (windowDpi && (windowDpi < 48 || windowDpi > 960))
        return false;
    if (active < 0 || active > 1 || !std::isfinite(split) || split < .1 || split > .9 || recent.size() > 10)
        return false;
    std::set<Id> all;
    Id max = 0;
    for (auto &p : panes) {
        if (p.tabs.empty() || p.tabs.size() > 4096)
            return false;
        std::set<Id> groups, finished;
        for (auto &g : p.groups) {
            if (!g.id || g.name.empty() || !all.insert(g.id).second)
                return false;
            groups.insert(g.id);
            max = std::max(max, g.id);
        }
        bool found = false;
        Id last = 0;
        for (auto &t : p.tabs) {
            if (!t.id || !all.insert(t.id).second || t.path.empty() || t.history.empty() ||
                t.historyIndex < 0 || t.historyIndex >= (int)t.history.size())
                return false;
            if (t.name.empty() != t.home.empty())
                return false;
            if (t.group && !groups.contains(t.group))
                return false;
            if (last != t.group) {
                if (last)
                    finished.insert(last);
                if (t.group && finished.contains(t.group))
                    return false;
            }
            last = t.group;
            found |= t.id == p.selected;
            max = std::max(max, t.id);
        }
        if (!found)
            return false;
        for (auto g : groups)
            if (std::none_of(p.tabs.begin(), p.tabs.end(), [&](auto &t) { return t.group == g; }))
                return false;
    }
    return nextId > max;
}
} // namespace zen
