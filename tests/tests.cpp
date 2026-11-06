#include "storage.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <limits>
using namespace zen;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)
static void CheckTabActivation() {
    Model m;
    Id a = m.panes[0].selected;
    Id b = m.Add(0, L"C:/b", 0, -1, true, a);
    Id c = m.Add(0, L"C:/c", 0, -1, true, b);
    Id right = m.Add(0, L"C:/right", 0, -1, false);
    m.Remove(0, {c});
    CHECK(m.panes[0].selected == b);
    m.Remove(0, {b});
    CHECK(m.panes[0].selected == a && m.Find(right));

    // 多开目录都回共同来源；关闭非当前标签不切换。
    b = m.Add(0, L"C:/b", 0, 1, true, a);
    c = m.Add(0, L"C:/c", 0, 2, false, a);
    m.Remove(0, {c});
    CHECK(m.panes[0].selected == b);
    m.Remove(0, {b});
    CHECK(m.panes[0].selected == a);
    b = m.Add(0, L"C:/b", 0, 1, true, a);
    m.Remove(0, {a});
    m.Remove(0, {b});
    CHECK(m.panes[0].selected == right);

    // 来源隐藏时跳过；只剩隐藏标签时新增未分组的此电脑。
    Id g = m.MakeGroup(0, right);
    b = m.Add(0, L"C:/b", 0, -1, true, right);
    m.SetGroupCollapsed(0, g, true);
    m.Remove(0, {b});
    CHECK(m.panes[0].selected != right && m.Visible(0, m.panes[0].selected));
    CHECK(m.Find(m.panes[0].selected)->path == Home && m.FindGroup(0, g)->collapsed);
    auto visible = m.panes[0].selected;
    auto recent = m.recent;
    m.AssignGroup(visible, g);
    CHECK(m.panes[0].selected != visible && m.Visible(0, m.panes[0].selected));
    CHECK(m.recent == recent && m.FindGroup(0, g)->collapsed);

    // 整组关闭即使只剩一个成员也不追溯来源。
    Model groupClose;
    a = groupClose.panes[0].selected;
    b = groupClose.Add(0, L"C:/b", 0, -1, true, a);
    g = groupClose.MakeGroup(0, b);
    groupClose.Find(b)->opener = a;
    right = groupClose.Add(0, L"C:/right", 0, -1, false);
    groupClose.Remove(0, {b}, true, true);
    CHECK(groupClose.panes[0].selected == right);

    // 移动不回来源；跨栏及重新分组清除来源，同组内排序保留。
    Model moved;
    a = moved.panes[0].selected;
    b = moved.Add(0, L"C:/b", 0, -1, true, a);
    right = moved.Add(0, L"C:/right", 0, -1, false);
    moved.Move(b, 0, 1, 0);
    CHECK(moved.Find(b)->opener == a);
    moved.Move(b, 1, 1, 0);
    CHECK(moved.panes[0].selected == right && !moved.Find(b)->opener);
    c = moved.Add(1, L"C:/c", 0, -1, true, b);
    g = moved.MakeGroup(1, c);
    CHECK(!moved.Find(c)->opener);
    Id d = moved.Add(1, L"C:/d", g, -1, true, c);
    moved.MoveGroup(1, g, 0, 1);
    CHECK(!moved.Find(d)->opener && moved.panes[0].selected == d);
    Id hiddenGroup = moved.MakeGroup(1, b);
    moved.SetGroupCollapsed(1, hiddenGroup, true);
    moved.Move(d, 1, 1, hiddenGroup);
    CHECK(moved.FindGroup(1, hiddenGroup)->collapsed && moved.Visible(1, moved.panes[1].selected));
    CHECK(moved.panes[1].selected != d && moved.Valid());
}
static void CheckSingleExpandedGroups() {
    Model m;
    CHECK(m.singleExpandedGroup);
    Id a = m.panes[0].selected;
    Id ga = m.MakeGroup(0, a);
    Id b = m.Add(0, L"C:/b");
    Id gb = m.MakeGroup(0, b);
    Id gr = m.MakeGroup(1, m.panes[1].selected);
    CHECK(m.FindGroup(0, ga)->collapsed && !m.FindGroup(0, gb)->collapsed);
    const auto countBeforeExpand = m.panes[0].tabs.size();
    m.SetGroupCollapsed(0, ga, false);
    Id fallback = m.panes[0].selected;
    CHECK(fallback != a && fallback != b && !m.Find(fallback)->group && m.Find(fallback)->path == Home);
    CHECK(m.panes[0].tabs.size() == countBeforeExpand + 1 && m.FindGroup(0, gb)->collapsed);
    CHECK(!m.FindGroup(1, gr)->collapsed);
    auto snapshot = m.CaptureGroups(0);
    m.SetGroupCollapsed(0, gb, false);
    CHECK(m.panes[0].selected == fallback);
    m.RestoreGroups(snapshot);
    CHECK(m.panes[0].selected == fallback && !m.FindGroup(0, ga)->collapsed && m.FindGroup(0, gb)->collapsed);
    m.SetSingleExpandedGroup(false);
    CHECK(!m.groupDoubleClick);
    m.groupDoubleClick = true;
    m.SetGroupCollapsed(0, gb, false);
    CHECK(!m.FindGroup(0, ga)->collapsed && !m.FindGroup(0, gb)->collapsed);
    m.panes[0].selected = a;
    m.SetSingleExpandedGroup(true);
    CHECK(!m.FindGroup(0, ga)->collapsed && m.FindGroup(0, gb)->collapsed);
    Id ungrouped = m.Add(0, Home);
    m.SetGroupCollapsed(0, gb, false);
    CHECK(m.panes[0].selected == ungrouped);
    m.SetGroupCollapsed(0, gb, true);
    CHECK(m.FindGroup(0, ga)->collapsed && m.FindGroup(0, gb)->collapsed);
    m.Add(0, Home, ga);
    CHECK(!m.FindGroup(0, ga)->collapsed);
    // 不携带当前标签时，目标栏原选择及其展开组优先。
    m.MoveGroup(0, gb, 1, 1);
    CHECK(m.FindGroup(1, gb)->collapsed && !m.FindGroup(1, gr)->collapsed);
    m.MoveGroup(0, ga, 1, 1);
    CHECK(!m.FindGroup(1, ga)->collapsed && m.FindGroup(1, gr)->collapsed);

    // 当前格式保存开关；旧格式明确拒绝，不迁移。
    m.SetSingleExpandedGroup(false);
    m.SetGroupCollapsed(1, gr, false);
    m.panes[1].selected = a;
    auto file = ConfigDir() / L"groups.dat";
    CHECK(SaveModel(m, file));
    Model loaded;
    bool recovered = false, future = false;
    CHECK(LoadModel(loaded, file, recovered, future) && !loaded.singleExpandedGroup);
    CHECK(loaded.groupDoubleClick);
}
static void CheckGroupSelectionMemory() {
    Model m;
    Id a = m.panes[0].selected;
    Id ga = m.MakeGroup(0, a);
    Id b = m.Add(0, L"C:/b", ga);
    Id other = m.Add(0, Home);
    Id gb = m.MakeGroup(0, other);
    m.SetGroupCollapsed(0, ga, false);
    Id fallback = m.panes[0].selected;
    m.panes[0].selected = b;
    m.SetGroupCollapsed(0, gb, false);
    CHECK(m.FindGroup(0, ga)->lastSelected == b && m.panes[0].selected == fallback);
    m.SetGroupCollapsed(0, ga, false);
    CHECK(m.Visible(0, m.panes[0].selected));
    m.SetSingleExpandedGroup(false);
    m.SetGroupCollapsed(0, gb, false);
    m.panes[0].selected = other;
    m.SetGroupCollapsed(0, ga, true);
    m.SetGroupCollapsed(0, ga, false);
    CHECK(m.panes[0].selected == other);
    m.panes[0].selected = b;
    auto before = m.CaptureGroups(0);
    m.panes[0].selected = a;
    m.SetGroupCollapsed(0, ga, true);
    m.RestoreGroups(before);
    CHECK(m.panes[0].selected == b && m.FindGroup(0, ga)->lastSelected == b);
    m.SetGroupCollapsed(0, ga, true);
    auto file = ConfigDir() / L"group-selection.dat";
    CHECK(SaveModel(m, file));
    Model loaded;
    bool recovered = false, future = false;
    CHECK(LoadModel(loaded, file, recovered, future));
    Id selectedBeforeExpand = loaded.panes[0].selected;
    loaded.SetGroupCollapsed(0, ga, false);
    CHECK(loaded.panes[0].selected == selectedBeforeExpand);
    loaded.SetGroupCollapsed(0, ga, true);
    loaded.Remove(0, {b});
    loaded.SetGroupCollapsed(0, ga, false);
    CHECK(loaded.panes[0].selected == selectedBeforeExpand);
    m.SetGroupCollapsed(0, ga, false);
    m.AssignGroup(b, 0);
    m.AssignGroup(b, ga);
    m.SetGroupCollapsed(0, ga, false);
    m.SetGroupCollapsed(0, ga, true);
    Id fresh = m.Add(0, Home, ga);
    CHECK(m.panes[0].selected == fresh);
    m.SetGroupCollapsed(0, ga, true);
    m.MoveGroup(0, ga, 1, 1);
    Id rightBefore = m.panes[1].selected;
    m.SetGroupCollapsed(1, ga, false);
    CHECK(m.panes[1].selected == rightBefore && m.Valid());
}
static void CheckGroupColors() {
    Model m;
    std::vector<Id> groups;
    for (const auto &preset : GroupColors) {
        Id g = m.MakeGroup(0, m.Add(0, Home));
        groups.push_back(g);
        CHECK(m.FindGroup(0, g)->color == preset.color);
    }
    // 另一栏独立；折叠组仍占颜色；全部用过按次数、固定顺序分配。
    Id right = m.MakeGroup(1, m.panes[1].selected);
    CHECK(m.FindGroup(1, right)->color == GroupColors[0].color);
    Id repeated = m.MakeGroup(0, m.Add(0, Home));
    CHECK(m.FindGroup(0, repeated)->color == GroupColors[0].color);
    Id next = m.MakeGroup(0, m.Add(0, Home));
    CHECK(m.FindGroup(0, next)->color == GroupColors[1].color);
    m.FindGroup(0, groups[2])->color = RGB(1, 2, 3);
    Id available = m.MakeGroup(0, m.Add(0, Home));
    CHECK(m.FindGroup(0, available)->color == GroupColors[2].color);
    auto file = ConfigDir() / L"colors.dat";
    CHECK(SaveModel(m, file));
    Model loaded;
    bool recovered = false, future = false;
    CHECK(LoadModel(loaded, file, recovered, future));
    CHECK(loaded.FindGroup(0, groups[2])->color == RGB(1, 2, 3));
    CHECK(loaded.FindGroup(0, available)->color == GroupColors[2].color);
}
static void CheckLocationText() {
    Model model;
    // 模拟设备提供程序的解析名称；不依赖设备型号、实际连接或名称前缀特判。
    const std::wstring device = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\"
                                L"\\\\?\\usb#vid_1234&pid_5678#device\\{storage-id}";
    model.locations.Set(device, {device, L"内部存储设备", {}});
    model.locations.Set(L"zen-shell:another-device", {L"", L"SD 卡", {2, 0, 0, 0}});
    CHECK(model.locations.DisplayPath(device) == L"内部存储设备");
    CHECK(model.locations.FileSystemPath(device).empty());
    CHECK(model.locations.Breadcrumbs(device).front().first == L"内部存储设备");
    CHECK(model.locations.DisplayPath(L"zen-shell:another-device") == L"SD 卡");
    CHECK(model.locations.DisplayPath(Home) == L"此电脑");
    for (const auto *path : {L"C:\\", L"d:/work/child", L"C:\\work\\child", L"\\\\server\\share\\child"}) {
        model.locations.Set(path, {path, L"只显示末级名称会丢失路径", {}});
        CHECK(model.locations.DisplayPath(path) == path);
        CHECK(model.locations.FileSystemPath(path) == path);
    }
    for (const auto *path : {L"", L"C:", L"C:relative", L"::\\device", L"1:\\invalid", L"https://host"})
        CHECK(!IsAbsoluteDrivePath(path));
    auto id = model.Add(0, device);
    model.Remove(0, {id}, true);
    CHECK(model.recent.front() == device);
    CHECK(model.locations.DisplayPath(model.recent.front()) == L"内部存储设备");
    const auto file = ConfigDir() / L"recent-device.dat";
    CHECK(SaveModel(model, file));
    Model loaded;
    bool recovered = false, future = false;
    CHECK(LoadModel(loaded, file, recovered, future));
    CHECK(loaded.recent.front() == device);
    CHECK(loaded.locations.DisplayPath(loaded.recent.front()) == L"内部存储设备");
    CHECK(loaded.locations.FileSystemPath(loaded.recent.front()).empty());
}
static void CheckLocationStorage() {
    Model m;
    auto id = m.panes[0].selected;
    std::wstring key = L"zen-shell:device-folder";
    ShellLocation location{L"", L"内部存储设备", {2, 0, 0, 0}, false};
    m.locations.Set(key, location);
    auto *tab = m.Find(id);
    tab->path = tab->home = key;
    tab->name = L"手机";
    tab->history = {Home, key};
    tab->historyIndex = 1;
    m.recent = {key};
    auto file = ConfigDir() / L"locations.dat";
    CHECK(SaveModel(m, file));
    Model loaded;
    bool recovered = false, future = false;
    CHECK(LoadModel(loaded, file, recovered, future));
    CHECK(loaded.Find(id)->path == key && loaded.Find(id)->home == key);
    CHECK(loaded.Find(id)->history == tab->history && loaded.recent == m.recent);
    CHECK((*loaded.locations.Find(key)).pidl == location.pidl);
    CHECK((*loaded.locations.Find(key)).displayName == location.displayName);
    CHECK((*loaded.locations.Find(key)).parsingName.empty() && (*loaded.locations.Find(key)).usable);
    CHECK(ValidPidl({0, 0}) && !ValidPidl({0}) && !ValidPidl({1, 0, 0, 0}));
    CHECK(!ValidPidl({2, 0}) && !ValidPidl({0, 0, 0, 0}));
    auto invalid = location;
    invalid.pidl = {255, 255, 0, 0};
    m.locations.Set(key, invalid);
    CHECK(!SaveModel(m, file));
    CHECK(LoadModel(loaded, file, recovered, future) && !recovered);
    CHECK((*loaded.locations.Find(key)).pidl == location.pidl);
}
static void CheckIndependentTabs() {
    Model m;
    auto id = m.panes[0].selected;
    m.CommitNavigation(id, L"C:\\home", {L"C:\\home"});
    m.Rename(id, L"常用工作目录");
    auto group = m.MakeGroup(0, id);
    m.FindGroup(0, group)->name = L"工作";
    m.CommitNavigation(id, L"C:\\elsewhere", {L"C:\\elsewhere"});
    auto file = ConfigDir() / L"independent.dat";
    CHECK(SaveModel(m, file));
    auto tabs = TabsPath(file);
    auto originalTabs = ReadEnvelope(tabs, 0x544E455A, 2);
    std::filesystem::remove(file);
    Model loaded;
    bool recovered = false, future = false;
    CHECK(LoadModel(loaded, file, recovered, future) && !future && !recovered);
    CHECK(loaded.Find(id)->name == L"常用工作目录" && loaded.Find(id)->path == L"C:\\home");
    CHECK(loaded.Find(id)->group == group && loaded.FindGroup(0, group)->name == L"工作");
    CHECK(ReadEnvelope(tabs, 0x544E455A, 2) == originalTabs);
    // 标签文件损坏只回退自己的备份；附属配置不参与标签定义恢复。
    CHECK(SaveModel(m, file));
    std::ofstream(tabs, std::ios::binary | std::ios::trunc) << "broken";
    CHECK(LoadModel(loaded, file, recovered, future) && recovered && !future);
    CHECK(loaded.Find(id)->name == m.Find(id)->name);
    CHECK(SaveModel(loaded, file));
    CHECK(LoadModel(loaded, file, recovered, future) && !recovered && !future);
    auto current = ReadEnvelope(tabs, 0x544E455A, 2);
    CHECK(WriteEnvelope(tabs, 0x544E455A, current, false, 3));
    CHECK(!LoadModel(loaded, file, recovered, future) && future);
    CHECK(!SaveModel(m, file));
    CHECK(ReadEnvelope(tabs, 0x544E455A, 3) == current);
}
static void CheckQuickAccess() {
    Model m;
    auto tab = m.Add(0, L"C:\\quick-only");
    auto entry = m.quickAccess.Put(m.locations, m.Find(tab)->path, L"工作");
    CHECK(entry && m.quickAccess.links.size() == 1);
    CHECK(m.quickAccess.Put(m.locations, m.Find(tab)->path, L"新名称") == entry);
    CHECK(m.quickAccess.links.size() == 1 && m.quickAccess.links[0].name == L"新名称");
    auto second = m.quickAccess.Put(m.locations, Home, L"起点");
    CHECK(second && m.quickAccess.Move(1, -1) && m.quickAccess.links[0].id == second);
    CHECK(!m.quickAccess.Move(0, -1));
    auto file = ConfigDir() / L"quick-access-test.dat";
    CHECK(m.quickAccess.Save(m.locations, file));
    m.Remove(0, {tab}, false);
    auto session = ConfigDir() / L"quick-only-session.dat";
    CHECK(SaveModel(m, session));
    Model loaded;
    bool recovered = false, future = false;
    CHECK(LoadModel(loaded, session, recovered, future));
    CHECK(loaded.locations.Find(L"C:\\quick-only"));
    // 独立配置可在没有标签会话的情况下恢复，并保留顺序与目录身份。
    Model independent;
    independent.quickAccess.Load(independent.locations, file);
    CHECK(!independent.quickAccess.readOnly && independent.quickAccess.links.size() == 2);
    CHECK(independent.quickAccess.links[0].id == second && independent.quickAccess.links[1].id == entry);
    CHECK(independent.locations.EnsureId(independent.quickAccess.links[1].target) ==
          m.locations.EnsureId(L"C:\\quick-only"));
    CHECK(independent.quickAccess.Remove(1));
    CHECK(independent.quickAccess.Save(independent.locations, file));
    CHECK(independent.quickAccess.Save(independent.locations, file));
    CHECK(std::distance(std::filesystem::directory_iterator(DataDir() / L"quick-access" / file.stem()),
                        std::filesystem::directory_iterator{}) == 2);
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "broken";
    QuickAccess backup;
    backup.Load(independent.locations, file);
    CHECK(!backup.readOnly && backup.links.size() == 1 && backup.links[0].id == second);
    CHECK(backup.Save(independent.locations, file));
    auto bytes = ReadEnvelope(file, 0x514E455A);
    CHECK(WriteEnvelope(file, 0x514E455A, bytes, false, 2));
    QuickAccess newer;
    newer.Load(independent.locations, file);
    CHECK(newer.readOnly && !newer.Save(independent.locations, file));
}
static void CheckLocationReferencesAndCleanup() {
    Model m;
    auto first = m.Add(0, L"C:\\shared-location");
    auto second = m.Add(0, L"C:\\shared-location");
    m.Rename(first, L"保留的主目录标签");
    auto group = m.MakeGroup(0, first);
    auto file = ConfigDir() / L"stable-ids.dat";
    CHECK(SaveModel(m, file));
    auto shared = m.locations.EnsureId(m.Find(first)->path);
    CHECK(shared == m.locations.EnsureId(m.Find(second)->path));
    auto bytes = ReadEnvelope(TabsPath(file), 0x544E455A, 2);
    Reader reader{bytes};
    auto generation = reader.Pod<GUID>();
    auto recordsFile = LocationsPath(file, generation);
    // 关系文件中不再嵌入目录解析名称；位置表中同一目录只存一份。
    Writer needle;
    needle.Text(L"C:\\shared-location");
    CHECK(std::search(bytes.begin(), bytes.end(), needle.bytes.begin(), needle.bytes.end()) == bytes.end());
    auto records = ReadEnvelope(recordsFile, 0x4C4E455A);
    Reader data{records};
    LocationStore store;
    store.ReadRecords(data);
    CHECK(store.Count() == 2 && store.EnsureId(L"C:\\shared-location") == shared);
    CHECK(m.CommitNavigation(first, L"D:\\corrected", {L"D:\\corrected"}));
    CHECK(SaveModel(m, file));
    CHECK(m.locations.EnsureId(m.Find(first)->path) != shared);
    CHECK(m.locations.EnsureId(m.Find(second)->path) == shared);
    CHECK(m.locations.EnsureId(m.Find(first)->home) == shared);
    // 主资料缺失时使用上一份资料补齐同一 ID，不能恢复成旧标签结构。
    bytes = ReadEnvelope(TabsPath(file), 0x544E455A, 2);
    Reader head{bytes};
    auto current = head.Pod<GUID>();
    std::filesystem::remove(LocationsPath(file, current));
    Model loaded;
    bool recovered = false, future = false;
    CHECK(LoadModel(loaded, file, recovered, future));
    CHECK(loaded.Find(first)->name == m.Find(first)->name && loaded.Find(first)->group == group);
    CHECK(loaded.Find(second)->path == L"C:\\shared-location");
    CHECK(loaded.locations.EnsureId(loaded.Find(first)->path) == m.locations.EnsureId(m.Find(first)->path));
    CHECK(loaded.Find(first)->path != Home);
    // 两份目录资料均缺失，仍保留标签和组及不可解析的 ID 引用。
    std::filesystem::remove(recordsFile);
    CHECK(LoadModel(loaded, file, recovered, future));
    CHECK(loaded.Find(first)->name == m.Find(first)->name && loaded.Find(first)->group == group);
    CHECK(loaded.locations.EnsureId(loaded.Find(second)->path) == shared);
    CHECK(loaded.Find(second)->path != Home);
    // 历史和最近关闭仍是有效引用；移除后只剩此电脑，备份轮换后旧资料被回收。
    m.Remove(0, {first, second}, true);
    CHECK(SaveModel(m, file));
    CHECK(LoadModel(loaded, file, recovered, future) && !loaded.recent.empty());
    CHECK(loaded.locations.Find(L"C:\\shared-location"));
    m.recent.clear();
    GUID orphan{};
    CHECK(SUCCEEDED(CoCreateGuid(&orphan)));
    CHECK(WriteEnvelope(LocationsPath(file, orphan), 0x4C4E455A, records, false));
    CHECK(SaveModel(m, file));
    CHECK(!std::filesystem::exists(LocationsPath(file, orphan)));
    CHECK(SaveModel(m, file));
    CHECK(LoadModel(loaded, file, recovered, future) && loaded.locations.Count() == 1);
    CHECK(std::distance(std::filesystem::directory_iterator(DataDir() / L"locations" / L"stable-ids"),
                        std::filesystem::directory_iterator{}) == 2);
    CHECK(std::distance(std::filesystem::directory_iterator(DataDir() / L"history" / L"stable-ids"),
                        std::filesystem::directory_iterator{}) == 2);
}
int main(int argc, char **argv) {
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--check-storage") {
        auto root = std::filesystem::absolute(argv[2]);
        SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", root.c_str());
        Model m;
        bool recovered = false, future = false;
        if (!LoadModel(m, ConfigDir() / L"session.dat", recovered, future) || recovered || future)
            return 1;
        for (int p = 0; p < 2; ++p) {
            size_t named = 0;
            for (const auto &t : m.panes[p].tabs)
                named += !t.name.empty();
            std::cout << "pane=" << p << " tabs=" << m.panes[p].tabs.size()
                      << " groups=" << m.panes[p].groups.size() << " named=" << named << "\n";
        }
        if (argc == 4) {
            auto output = std::filesystem::absolute(argv[3]);
            if (output == root)
                return 1;
            SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", output.c_str());
            if (!SaveModel(m, ConfigDir() / L"session.dat"))
                return 1;
        }
        return m.Valid() ? 0 : 1;
    }
    auto portable =
        std::filesystem::current_path() / L".test-data" /
        (L"portable-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", portable.c_str());
    SetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", nullptr);
    try {
        SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", nullptr);
        CHECK(ConfigDir() == ExeDir() / L"config" && DataDir() == ExeDir() / L"data" &&
              LogDir() == ExeDir() / L"log");
        SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", portable.c_str());
        CheckTabActivation();
        auto crumbs = Breadcrumbs(L"C:\\work\\child");
        CHECK(crumbs.size() == 4 && crumbs[0].second == Home && crumbs[1].second == L"C:\\" &&
              crumbs.back().second == L"C:\\work\\child");
        CHECK(Breadcrumbs(Home).size() == 1);
        auto unc = Breadcrumbs(L"\\\\server\\share\\child");
        CHECK(unc.size() == 4 && unc[1].second == L"\\\\server" && unc[2].second == L"\\\\server\\share");
        CHECK(InspectLocalPath(L"\\\\server\\share") == LocalPathState::Deferred);
        CHECK(InspectLocalPath(Home) == LocalPathState::Deferred);
        CHECK(!CanNavigateUp(Home));
        CHECK(!CanNavigateUp(L"::{00021400-0000-0000-C000-000000000046}"));
        CHECK(CanNavigateUp(L"C:\\"));
        Model m;
        CHECK(m.Valid());
        Id a = m.panes[0].selected;
        Id b = m.Add(0, L"C:\\b");
        Id c = m.Add(0, L"C:\\c");
        Id g = m.MakeGroup(0, a);
        m.AssignGroup(b, g);
        m.AssignGroup(c, g);
        CHECK(m.Valid());
        m.AssignGroup(b, 0);
        CHECK(m.panes[0].tabs[0].id == a && m.panes[0].tabs[1].id == c && m.panes[0].tabs[2].id == b);
        CHECK(m.Valid());
        m.Rename(a, L"项目");
        CHECK(m.Find(a)->home == Home);
        CHECK(m.CommitNavigation(a, L"C:\\other", {L"C:\\other"}));
        m.Rename(a, L"项目二");
        CHECK(m.Find(a)->home == Home);
        m.panes[0].selected = a;
        m.Remove(0, {a});
        CHECK(m.panes[0].selected == c);
        CHECK(m.recent[0] == L"C:\\other");
        m.Move(c, 1, 1, 0);
        CHECK(m.PaneOf(c) == 1 && m.active == 1 && m.Valid());
        Id group = m.MakeGroup(1, c);
        Id d = m.Add(1, L"D:\\", group, 2);
        m.panes[1].selected = c;
        m.MoveGroup(1, group, 0, 0);
        CHECK(m.active == 0 && m.panes[0].selected == c && m.Valid());
        m.Record({L"C:\\A", L"c:\\a", L"C:\\B"});
        CHECK(m.recent[0] == L"C:\\A" && m.recent[1] == L"C:\\B");
        for (int i = 0; i < 20; ++i)
            m.Record({L"C:\\" + std::to_wstring(i)});
        CHECK(m.recent.size() == 10);
        Model passive;
        Id selectedLeft = passive.panes[0].selected, selectedRight = passive.panes[1].selected;
        Id grouped = passive.Add(0, L"C:\\grouped", 0, -1, false);
        Id gid = passive.MakeGroup(0, grouped);
        passive.FindGroup(0, gid)->collapsed = true;
        int activeBefore = passive.active;
        passive.MoveGroup(0, gid, 1, 1);
        CHECK(passive.panes[0].selected == selectedLeft && passive.panes[1].selected == selectedRight);
        CHECK(passive.active == activeBefore && passive.FindGroup(1, gid)->collapsed &&
              passive.recent.empty() && passive.Valid());
        passive.Move(selectedLeft, 1, 2, 0);
        CHECK(passive.panes[0].tabs.size() == 1 && passive.panes[0].tabs[0].path == Home && passive.Valid());
        passive.split = std::numeric_limits<double>::quiet_NaN();
        CHECK(!passive.Valid());
        // 折叠当前组跳过隐藏标签，优先右侧，再左侧，最后新建此电脑。
        Model folded;
        Id left = folded.panes[0].selected;
        Id inside = folded.Add(0, L"C:/inside");
        Id foldGroup = folded.MakeGroup(0, inside);
        Id hidden = folded.Add(0, L"C:/hidden");
        Id hiddenGroup = folded.MakeGroup(0, hidden);
        folded.SetGroupCollapsed(0, hiddenGroup, true);
        Id right = folded.Add(0, Home);
        folded.panes[0].selected = inside;
        folded.active = 1;
        folded.SetGroupCollapsed(0, foldGroup, true);
        CHECK(folded.panes[0].selected == right && folded.active == 1);
        folded.SetGroupCollapsed(0, foldGroup, false);
        CHECK(folded.panes[0].selected == right);
        folded.Remove(0, {right});
        folded.panes[0].selected = inside;
        folded.SetGroupCollapsed(0, foldGroup, true);
        CHECK(folded.panes[0].selected == left);
        folded.Remove(0, {left});
        folded.panes[0].selected = inside;
        auto beforeCount = folded.panes[0].tabs.size();
        auto beforeRecent = folded.recent;
        folded.SetGroupCollapsed(0, foldGroup, true);
        auto *fallback = folded.Find(folded.panes[0].selected);
        CHECK(fallback && fallback->path == Home && !fallback->group);
        CHECK(folded.panes[0].tabs.size() == beforeCount && folded.recent == beforeRecent);
        CHECK(folded.Find(inside) && folded.Find(hidden) && folded.Valid());
        auto stable = folded.panes[0].selected;
        folded.SetGroupCollapsed(0, hiddenGroup, true);
        folded.SetGroupCollapsed(0, 999999, true);
        folded.SetGroupCollapsed(-1, foldGroup, true);
        CHECK(folded.panes[0].selected == stable && folded.active == 1);
        auto dir = std::filesystem::current_path() / L".test-data";
        // 整组关闭不激活折叠组中的隐藏标签，取消分组不改变选择。
        Model closing;
        Id only = closing.panes[0].selected;
        Id onlyGroup = closing.MakeGroup(0, only);
        Id otherPane = closing.panes[1].selected;
        closing.Ungroup(0, onlyGroup);
        CHECK(closing.panes[0].selected == only && !closing.Find(only)->group);
        closing.MakeGroup(0, only);
        closing.Remove(0, {only}, true, true);
        auto replacement = closing.panes[0].selected;
        CHECK(closing.panes[0].tabs.size() == 1 && closing.Find(replacement)->path == Home &&
              !closing.Find(replacement)->group && closing.panes[0].groups.empty());
        Id hiddenTab = closing.Add(0, L"C:/hidden", 0, -1, false);
        Id hiddenId = closing.MakeGroup(0, hiddenTab);
        closing.SetGroupCollapsed(0, hiddenId, true);
        closing.MakeGroup(0, replacement);
        closing.Remove(0, {replacement}, true, true);
        CHECK(closing.FindGroup(0, hiddenId)->collapsed && closing.Find(hiddenTab));
        CHECK(closing.panes[0].selected != hiddenTab &&
              closing.Find(closing.panes[0].selected)->path == Home &&
              !closing.Find(closing.panes[0].selected)->group && closing.panes[1].selected == otherPane &&
              closing.Valid());
        auto selectedBefore = closing.panes[0].selected;
        closing.Remove(0, {hiddenTab}, true, true);
        CHECK(closing.panes[0].selected == selectedBefore && closing.Valid());
        CHECK(ConfigDir() == portable / L"config" && DataDir() == portable / L"data" &&
              LogDir() == portable / L"log");
        Log(L"默认关闭日志");
        CHECK(!std::filesystem::exists(LogDir()));
        SetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", L"1");
        Log(L"显式开启日志");
        CHECK(std::filesystem::file_size(LogDir() / L"application.log") > 0);
        SetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", nullptr);
        auto logSize = std::filesystem::file_size(LogDir() / L"application.log");
        Log(L"关闭后不写入");
        CHECK(std::filesystem::file_size(LogDir() / L"application.log") == logSize);
        auto file = ConfigDir() / L"session.dat";
        std::filesystem::create_directories(dir);
        CHECK(InspectLocalPath(std::filesystem::absolute(dir).wstring()) == LocalPathState::Exists);
        CHECK(
            InspectLocalPath((std::filesystem::absolute(dir) / L"missing-test-directory-981721").wstring()) ==
            LocalPathState::Missing);
        m.Find(d)->displayName = L"数据盘 (D:)";
        m.Find(d)->opener = c;
        m.Find(c)->displayName = L"系统显示名称";
        m.Rename(c, L"固定名称优先");
        CHECK(m.Find(c)->Title() == L"固定名称优先");
        CHECK(SaveModel(m, file));
        m.wrap = false;
        CHECK(SaveModel(m, file));
        Model loaded;
        bool recovered = false, future = false;
        CHECK(LoadModel(loaded, file, recovered, future));
        CHECK(!loaded.wrap && loaded.Valid() && !recovered && loaded.windowDpi == m.windowDpi);
        CHECK(loaded.recent == m.recent && loaded.Find(c)->history == m.Find(c)->history);
        CHECK(loaded.Find(c)->historyIndex == m.Find(c)->historyIndex);
        CHECK(std::distance(std::filesystem::directory_iterator(DataDir() / L"history" / L"session"),
                            std::filesystem::directory_iterator{}) == 2);
        auto blockedConfig = ConfigDir() / L"blocked.dat";
        std::filesystem::create_directories(blockedConfig);
        CHECK(!SaveModel(m, blockedConfig));
        CHECK(std::distance(std::filesystem::directory_iterator(DataDir() / L"history" / L"session"),
                            std::filesystem::directory_iterator{}) == 2);
        CHECK(LoadModel(loaded, file, recovered, future) && !recovered && !loaded.wrap);
        CHECK(!loaded.Find(d)->opener);
        CHECK(loaded.Find(d)->Title() == L"数据盘 (D:)" && loaded.Find(d)->name.empty());
        CHECK(loaded.Find(c)->Title() == L"固定名称优先");
        loaded.Rename(c, L"");
        CHECK(loaded.Find(c)->Title() == L"系统显示名称");
        CHECK(loaded.CommitNavigation(d, L"D:\\", {L"D:\\", false}));
        CHECK(loaded.Find(d)->displayName == L"数据盘 (D:)");
        CHECK(loaded.CommitNavigation(d, L"D:\\other", {L"D:\\other"}));
        CHECK(loaded.Find(d)->displayName.empty());
        // 历史丢失时必须恢复同一代配置和历史，不能与其他快照混用。
        auto configBytes = ReadEnvelope(file, 0x434E455A, 7);
        Reader configReader{configBytes};
        auto generation = configReader.Pod<GUID>();
        wchar_t generationName[40]{};
        StringFromGUID2(generation, generationName, 40);
        auto historyFile = DataDir() / L"history" / L"session" / (std::wstring(generationName) + L".dat");
        std::filesystem::remove(historyFile);
        CHECK(LoadModel(loaded, file, recovered, future) && !recovered && !loaded.wrap);
        CHECK(loaded.recent.empty());
        CHECK(SaveModel(m, file));
        CHECK(SaveModel(m, file));
        CHECK(std::distance(std::filesystem::directory_iterator(DataDir() / L"history" / L"session"),
                            std::filesystem::directory_iterator{}) == 2);
        // 为下面的损坏恢复用例建立 wrap=true 的有效备份。
        m.wrap = true;
        CHECK(SaveModel(m, file));
        m.wrap = false;
        CHECK(SaveModel(m, file));

        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        CloseHandle(h);
        CHECK(LoadModel(loaded, file, recovered, future) && !recovered && loaded.wrap);
        // 有效备份存在时也不降级加载未来版本，避免旧程序覆盖新格式。
        CHECK(SaveModel(m, file));
        h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        SetFilePointer(h, 4, nullptr, FILE_BEGIN);
        DWORD written = 0;
        uint32_t version = 8;
        CHECK(WriteFile(h, &version, sizeof(version), &written, nullptr) && written == sizeof(version));
        CloseHandle(h);
        CHECK(LoadModel(loaded, file, recovered, future) && future && !recovered);
        CHECK(loaded.Find(c)->name == m.Find(c)->name);
        std::vector<uint8_t> badBool{2};
        Reader reader{badBool};
        bool rejected = false;
        try {
            (void)reader.Pod<bool>();
        } catch (...) {
            rejected = true;
        }
        CHECK(rejected);
        auto backup = file;
        backup += L".bak";
        std::filesystem::remove(file);
        std::filesystem::remove(backup);
        CHECK(m.Find(d) != nullptr);
        CheckLocationStorage();
        CheckIndependentTabs();
        CheckLocationReferencesAndCleanup();
        CheckQuickAccess();
        CheckLocationText();
        CheckSingleExpandedGroups();
        CheckGroupColors();
        CheckGroupSelectionMemory();
        std::cout << "model, grouping, history, recent, atomic persistence and backup tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
