#pragma once
#include "platform.h"
namespace zen {
using Id = uint64_t;
struct Column {
    PROPERTYKEY key{};
    UINT width = 120;
};
struct Tab {
    Id id = 0, group = 0;
    // 仅运行期保留的来源标签，不写入会话文件。
    Id opener = 0;
    std::wstring path = Home, name, home;
    // 当前路径最后一次成功解析的系统显示名称，独立于用户固定名称。
    std::wstring displayName;
    std::wstring Title() const {
        return !name.empty() ? name : !displayName.empty() ? displayName : LocationName(path);
    }
    std::vector<std::wstring> history{Home};
    int historyIndex = 0;
    UINT viewMode = FVM_DETAILS;
    int iconSize = 16;
    std::vector<SORTCOLUMN> sort;
    std::vector<Column> columns;
};
struct Group {
    Id id = 0;
    std::wstring name = L"新建组";
    COLORREF color = RGB(50, 120, 220);
    bool collapsed = false;
};
struct Pane {
    std::vector<Tab> tabs;
    std::vector<Group> groups;
    Id selected = 0;
};
struct Model {
    Pane panes[2];
    int active = 0;
    double split = .5;
    bool wrap = true;
    RECT window{100, 100, 1380, 900};
    bool maximized = false;
    std::vector<std::wstring> recent;
    Id nextId = 1;
    Model();
    Tab *Find(Id id);
    const Tab *Find(Id id) const;
    Group *FindGroup(int pane, Id id);
    int PaneOf(Id id) const;
    int Index(int pane, Id id) const;
    Id Add(int pane, const std::wstring &path = Home, Id group = 0, int index = -1, bool activate = true,
           Id opener = 0);
    void Navigate(Id id, const std::wstring &path, bool append = true);
    void Rename(Id id, const std::wstring &name);
    Id MakeGroup(int pane, Id tab);
    void AssignGroup(Id tab, Id group);
    void SetGroupCollapsed(int pane, Id group, bool collapsed);
    void Ungroup(int pane, Id group);
    // 单个主动关闭优先回来源；整组关闭和移动只按可见相邻标签补选。
    void Remove(int pane, const std::vector<Id> &ids, bool record = true, bool wholeGroup = false);
    bool Visible(int pane, Id tab) const;
    void EnsureVisibleSelection(int pane);
    void Move(Id id, int target, int index, Id group);
    void MoveGroup(int source, Id group, int target, int index);
    void Cleanup(int pane);
    void Record(const std::vector<std::wstring> &paths);
    bool Valid() const;
};
} // namespace zen
