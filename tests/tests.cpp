#include "storage.h"
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
    m.SetGroupCollapsed(0, ga, false);
    CHECK(m.panes[0].selected == a && m.FindGroup(0, gb)->collapsed);
    CHECK(!m.FindGroup(1, gr)->collapsed);
    auto snapshot = m.CaptureGroups(0);
    m.SetGroupCollapsed(0, gb, false);
    CHECK(m.panes[0].selected == b);
    m.RestoreGroups(snapshot);
    CHECK(m.panes[0].selected == a && !m.FindGroup(0, ga)->collapsed && m.FindGroup(0, gb)->collapsed);
    m.SetSingleExpandedGroup(false);
    m.SetGroupCollapsed(0, gb, false);
    CHECK(!m.FindGroup(0, ga)->collapsed && !m.FindGroup(0, gb)->collapsed);
    m.panes[0].selected = a;
    m.SetSingleExpandedGroup(true);
    CHECK(!m.FindGroup(0, ga)->collapsed && m.FindGroup(0, gb)->collapsed);
    m.Add(0, Home);
    m.SetGroupCollapsed(0, gb, false);
    CHECK(m.panes[0].selected == b);
    m.SetGroupCollapsed(0, gb, true);
    CHECK(m.FindGroup(0, ga)->collapsed && m.FindGroup(0, gb)->collapsed);
    m.Add(0, Home, ga);
    CHECK(!m.FindGroup(0, ga)->collapsed);
    // 不携带当前标签时，目标栏原选择及其展开组优先。
    m.MoveGroup(0, gb, 1, 1);
    CHECK(m.FindGroup(1, gb)->collapsed && !m.FindGroup(1, gr)->collapsed);
    m.MoveGroup(0, ga, 1, 1);
    CHECK(!m.FindGroup(1, ga)->collapsed && m.FindGroup(1, gr)->collapsed);

    // v2 保存开关；便携 v1 没有开关字段，默认启用并保留当前标签组。
    m.SetSingleExpandedGroup(false);
    m.SetGroupCollapsed(1, gr, false);
    m.panes[1].selected = a;
    auto file = ConfigDir() / L"groups.dat";
    CHECK(SaveModel(m, file));
    Model loaded;
    bool recovered = false, future = false;
    CHECK(LoadModel(loaded, file, recovered, future) && !loaded.singleExpandedGroup);
    auto bytes = ReadEnvelope(file, 0x434E455A, 4);
    bytes.resize(bytes.size() - sizeof(UINT));
    CHECK(WriteEnvelope(file, 0x434E455A, bytes, false, 3));
    CHECK(LoadModel(loaded, file, recovered, future) && loaded.windowDpi == 0);
    bytes.resize(bytes.size() - (m.panes[0].groups.size() + m.panes[1].groups.size()) * sizeof(Id));
    CHECK(WriteEnvelope(file, 0x434E455A, bytes, false, 2));
    CHECK(LoadModel(loaded, file, recovered, future) && !loaded.singleExpandedGroup);
    bytes.resize(bytes.size() - sizeof(bool));
    CHECK(WriteEnvelope(file, 0x434E455A, bytes, false, 1));
    CHECK(LoadModel(loaded, file, recovered, future) && loaded.singleExpandedGroup);
    CHECK(!loaded.FindGroup(1, ga)->collapsed && loaded.FindGroup(1, gr)->collapsed);
}
static void CheckGroupSelectionMemory() {
    Model m;
    Id a = m.panes[0].selected;
    Id ga = m.MakeGroup(0, a);
    Id b = m.Add(0, L"C:/b", ga);
    Id other = m.Add(0, Home);
    Id gb = m.MakeGroup(0, other);
    m.SetGroupCollapsed(0, ga, false);
    m.panes[0].selected = b;
    m.SetGroupCollapsed(0, gb, false);
    CHECK(m.FindGroup(0, ga)->lastSelected == b && m.panes[0].selected == other);
    m.SetGroupCollapsed(0, ga, false);
    CHECK(m.panes[0].selected == b);
    m.SetSingleExpandedGroup(false);
    m.SetGroupCollapsed(0, ga, true);
    m.SetGroupCollapsed(0, ga, false);
    CHECK(m.panes[0].selected == b);
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
    loaded.SetGroupCollapsed(0, ga, false);
    CHECK(loaded.panes[0].selected == b);
    loaded.SetGroupCollapsed(0, ga, true);
    loaded.Remove(0, {b});
    loaded.SetGroupCollapsed(0, ga, false);
    CHECK(loaded.panes[0].selected == a);
    m.SetGroupCollapsed(0, ga, false);
    m.AssignGroup(b, 0);
    m.AssignGroup(b, ga);
    m.SetGroupCollapsed(0, ga, false);
    m.SetGroupCollapsed(0, ga, true);
    Id fresh = m.Add(0, Home, ga);
    CHECK(m.panes[0].selected == fresh);
    m.SetGroupCollapsed(0, ga, true);
    m.MoveGroup(0, ga, 1, 1);
    m.SetGroupCollapsed(1, ga, false);
    CHECK(m.panes[1].selected == fresh && m.Valid());
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
int main() {
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
        m.Navigate(a, L"C:\\other");
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
        CHECK(folded.panes[0].selected == inside);
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
        CHECK(std::distance(std::filesystem::directory_iterator(DataDir() / L"history"),
                            std::filesystem::directory_iterator{}) == 2);
        auto blockedConfig = ConfigDir() / L"blocked.dat";
        std::filesystem::create_directories(blockedConfig);
        CHECK(!SaveModel(m, blockedConfig));
        CHECK(std::distance(std::filesystem::directory_iterator(DataDir() / L"history"),
                            std::filesystem::directory_iterator{}) == 2);
        CHECK(LoadModel(loaded, file, recovered, future) && !recovered && !loaded.wrap);
        CHECK(!loaded.Find(d)->opener);
        CHECK(loaded.Find(d)->Title() == L"数据盘 (D:)" && loaded.Find(d)->name.empty());
        CHECK(loaded.Find(c)->Title() == L"固定名称优先");
        loaded.Rename(c, L"");
        CHECK(loaded.Find(c)->Title() == L"系统显示名称");
        loaded.Navigate(d, L"D:\\", false);
        CHECK(loaded.Find(d)->displayName == L"数据盘 (D:)");
        loaded.Navigate(d, L"D:\\other");
        CHECK(loaded.Find(d)->displayName.empty());
        Model legacy;
        auto fixture = std::filesystem::path(ZEN_TEST_SOURCE_DIR) / "tests" / "fixtures" / "session-v1.dat";
        CHECK(!LoadModel(legacy, fixture, recovered, future) && !future && !recovered);

        // 历史丢失时必须恢复同一代配置和历史，不能与其他快照混用。
        auto configBytes = ReadEnvelope(file, 0x434E455A, 4);
        Reader configReader{configBytes};
        auto generation = configReader.Pod<GUID>();
        wchar_t generationName[40]{};
        StringFromGUID2(generation, generationName, 40);
        auto historyFile = DataDir() / L"history" / (std::wstring(generationName) + L".dat");
        std::filesystem::remove(historyFile);
        CHECK(LoadModel(loaded, file, recovered, future) && recovered && loaded.wrap);
        CHECK(loaded.recent == m.recent);
        CHECK(SaveModel(m, file));
        CHECK(SaveModel(m, file));
        CHECK(std::distance(std::filesystem::directory_iterator(DataDir() / L"history"),
                            std::filesystem::directory_iterator{}) == 2);
        // 为下面的损坏恢复用例建立 wrap=true 的有效备份。
        m.wrap = true;
        CHECK(SaveModel(m, file));
        m.wrap = false;
        CHECK(SaveModel(m, file));

        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        CloseHandle(h);
        CHECK(LoadModel(loaded, file, recovered, future) && recovered && loaded.wrap);
        // 有效备份存在时也不降级加载未来版本，避免旧程序覆盖新格式。
        CHECK(SaveModel(m, file));
        h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        SetFilePointer(h, 4, nullptr, FILE_BEGIN);
        DWORD written = 0;
        uint32_t version = 5;
        CHECK(WriteFile(h, &version, sizeof(version), &written, nullptr) && written == sizeof(version));
        CloseHandle(h);
        CHECK(!LoadModel(loaded, file, recovered, future) && future && !recovered);
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
