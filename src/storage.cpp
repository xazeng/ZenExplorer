#include "storage.h"
#include <fstream>
#include <set>
namespace zen {
static uint32_t Hash(const std::vector<uint8_t> &b) {
    uint32_t h = 2166136261u;
    for (auto c : b)
        h = (h ^ c) * 16777619u;
    return h;
}
bool WriteEnvelope(const std::filesystem::path &p, uint32_t magic, const std::vector<uint8_t> &b, bool backup,
                   uint32_t version) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    auto temp = p;
    temp += L".tmp";
    HANDLE f =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    uint32_t header[] = {magic, version, (uint32_t)b.size(), Hash(b)};
    DWORD n = 0;
    bool ok = WriteFile(f, header, sizeof(header), &n, nullptr) && n == sizeof(header);
    if (ok)
        ok = WriteFile(f, b.data(), (DWORD)b.size(), &n, nullptr) && n == b.size();
    if (ok)
        ok = FlushFileBuffers(f) != 0;
    CloseHandle(f);
    if (!ok)
        return false;
    if (backup && std::filesystem::exists(p)) {
        auto bak = p;
        bak += L".bak";
        ok = ReplaceFileW(p.c_str(), temp.c_str(), bak.c_str(), 0, nullptr, nullptr) != 0;
    } else
        ok = MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    return ok;
}
std::vector<uint8_t> ReadEnvelope(const std::filesystem::path &p, uint32_t magic, uint32_t maxVersion,
                                  uint32_t *version) {
    std::ifstream f(p, std::ios::binary);
    uint32_t h[4]{};
    if (!f.read((char *)h, sizeof(h)) || h[0] != magic)
        throw std::runtime_error("无效文件头");
    if (h[1] > maxVersion)
        throw std::domain_error("较新版本");
    if (h[1] < 1 || h[2] > 32 * 1024 * 1024)
        throw std::runtime_error("无效长度");
    std::vector<uint8_t> b(h[2]);
    if (!f.read((char *)b.data(), h[2]) || f.peek() != EOF || Hash(b) != h[3])
        throw std::runtime_error("校验失败");
    if (version)
        *version = h[1];
    return b;
}
// 新的拆分格式，不读取旧版混合会话。历史先落盘，配置再引用同代快照。
static constexpr uint32_t Magic = 0x434E455A;
static constexpr uint32_t HistoryMagic = 0x484E455A;
static constexpr uint32_t SessionVersion = 6, HistoryVersion = 2;
struct ObsoleteSession {};
static std::filesystem::path HistoryPath(const GUID &generation) {
    wchar_t name[40]{};
    StringFromGUID2(generation, name, 40);
    return DataDir() / L"history" / (std::wstring(name) + L".dat");
}
static std::filesystem::path ReferencedHistory(const std::filesystem::path &config) {
    uint32_t version = 0;
    auto bytes = ReadEnvelope(config, Magic, SessionVersion, &version);
    if (version != SessionVersion)
        throw ObsoleteSession{};
    Reader r{bytes};
    return HistoryPath(r.Pod<GUID>());
}
bool SaveModel(const Model &m, const std::filesystem::path &p) {
    if (!m.Valid()) {
        Log(L"拒绝保存无效状态");
        return false;
    }
    GUID generation{};
    if (FAILED(CoCreateGuid(&generation)))
        return false;
    Writer w, history;
    w.Pod(generation);
    // 四类位置只保存引用；同代历史文件中的位置表统一保存身份和显示信息。
    std::set<std::wstring> keys(m.recent.begin(), m.recent.end());
    for (const auto &pane : m.panes)
        for (const auto &tab : pane.tabs) {
            keys.insert(tab.path);
            if (!tab.home.empty())
                keys.insert(tab.home);
            keys.insert(tab.history.begin(), tab.history.end());
        }
    history.Pod((uint32_t)keys.size());
    for (const auto &key : keys) {
        ShellLocation fallback{key.starts_with(L"zen-shell:") ? L"" : key, LocationName(key), {}};
        auto found = m.locations.find(key);
        const auto &location = found == m.locations.end() ? fallback : found->second;
        if (key.size() > 32768 || location.parsingName.size() > 32768 ||
            location.displayName.size() > 32768 || (!location.pidl.empty() && !ValidPidl(location.pidl)) ||
            (location.pidl.empty() && location.parsingName.empty()))
            return false;
        history.Text(key);
        history.Text(location.parsingName);
        history.Text(location.displayName);
        history.Pod((uint32_t)location.pidl.size());
        history.bytes.insert(history.bytes.end(), location.pidl.begin(), location.pidl.end());
    }
    history.Pod((uint32_t)m.recent.size());
    for (const auto &s : m.recent)
        history.Text(s);
    history.Pod((uint32_t)(m.panes[0].tabs.size() + m.panes[1].tabs.size()));
    w.Pod(m.nextId);
    w.Pod(m.active);
    w.Pod(m.split);
    w.Pod(m.wrap);
    w.Pod(m.window);
    w.Pod(m.maximized);
    for (auto &pane : m.panes) {
        w.Pod(pane.selected);
        w.Pod((uint32_t)pane.groups.size());
        for (auto &g : pane.groups) {
            w.Pod(g.id);
            w.Text(g.name);
            w.Pod(g.color);
            w.Pod(g.collapsed);
        }
        w.Pod((uint32_t)pane.tabs.size());
        for (auto &t : pane.tabs) {
            w.Pod(t.id);
            w.Pod(t.group);
            w.Text(t.path);
            w.Text(t.name);
            w.Text(t.home);
            history.Pod(t.id);
            history.Pod(t.historyIndex);
            history.Pod((uint32_t)t.history.size());
            for (auto &s : t.history)
                history.Text(s);
            w.Pod(t.viewMode);
            w.Pod(t.iconSize);
            w.Pod((uint32_t)t.sort.size());
            for (auto &x : t.sort)
                w.Pod(x);
            w.Pod((uint32_t)t.columns.size());
            for (auto &x : t.columns)
                w.Pod(x);
            w.Text(t.displayName);
        }
    }
    w.Pod(m.singleExpandedGroup);
    for (const auto &pane : m.panes)
        for (const auto &g : pane.groups)
            w.Pod(g.lastSelected);
    w.Pod(m.windowDpi);
    w.Pod(m.groupDoubleClick);
    const auto historyPath = HistoryPath(generation);
    auto backup = p;
    backup += L".bak";
    std::filesystem::path obsoleteHistory;
    try {
        obsoleteHistory = ReferencedHistory(backup);
    } catch (...) {
    }
    if (history.bytes.size() > 32 * 1024 * 1024 || w.bytes.size() > 32 * 1024 * 1024)
        return false;
    if (!WriteEnvelope(historyPath, HistoryMagic, history.bytes, false, HistoryVersion))
        return false;
    if (!WriteEnvelope(p, Magic, w.bytes, true, SessionVersion)) {
        std::error_code ec;
        std::filesystem::remove(historyPath, ec);
        return false;
    }
    // 仅清理被本次替换的备份引用，保留当前与备份的同代历史。
    // 通用目录清理不在保存中执行，避免误删其他配置引用的历史。
    try {
        if (!obsoleteHistory.empty() && obsoleteHistory != historyPath &&
            obsoleteHistory != ReferencedHistory(backup)) {
            std::error_code ec;
            std::filesystem::remove(obsoleteHistory, ec);
        }
    } catch (...) {
    }
    return true;
}
static Model Decode(const std::vector<uint8_t> &b, uint32_t version) {
    if (version != SessionVersion)
        throw ObsoleteSession{};
    Reader r{b};
    uint32_t historyVersion = 0;
    auto historyBytes =
        ReadEnvelope(HistoryPath(r.Pod<GUID>()), HistoryMagic, HistoryVersion, &historyVersion);
    if (historyVersion != HistoryVersion)
        throw std::runtime_error("位置历史格式不匹配");
    Reader history{historyBytes};
    Model m;
    for (auto count = history.Count(1048576); count--;) {
        auto key = history.Text();
        ShellLocation location;
        location.parsingName = history.Text();
        location.displayName = history.Text();
        auto size = history.Count(1024 * 1024);
        if (size > history.bytes.size() - history.pos)
            throw std::runtime_error("位置标识截断");
        location.pidl.assign(history.bytes.begin() + history.pos, history.bytes.begin() + history.pos + size);
        history.pos += size;
        if (key.empty() || (!location.pidl.empty() && !ValidPidl(location.pidl)) ||
            (location.pidl.empty() && location.parsingName.empty()) ||
            !m.locations.emplace(key, std::move(location)).second)
            throw std::runtime_error("位置数据无效");
    }
    m.nextId = r.Pod<Id>();
    m.active = r.Pod<int>();
    m.split = r.Pod<double>();
    m.wrap = r.Pod<bool>();
    m.window = r.Pod<RECT>();
    m.maximized = r.Pod<bool>();
    m.recent.clear();
    for (auto n = history.Count(10); n--;)
        m.recent.push_back(history.Text());
    const auto historyCount = history.Count(8192);
    for (auto &p : m.panes) {
        p.tabs.clear();
        p.groups.clear();
        p.selected = r.Pod<Id>();
        for (auto n = r.Count(); n--;) {
            Group g;
            g.id = r.Pod<Id>();
            g.name = r.Text();
            g.color = r.Pod<COLORREF>();
            g.collapsed = r.Pod<bool>();
            p.groups.push_back(g);
        }
        for (auto n = r.Count(); n--;) {
            Tab t;
            t.id = r.Pod<Id>();
            t.group = r.Pod<Id>();
            t.path = r.Text();
            t.name = r.Text();
            t.home = r.Text();
            if (history.Pod<Id>() != t.id)
                throw std::runtime_error("历史与标签不匹配");
            t.historyIndex = history.Pod<int>();
            t.history.clear();
            for (auto h = history.Count(128); h--;)
                t.history.push_back(history.Text());
            t.viewMode = r.Pod<UINT>();
            t.iconSize = r.Pod<int>();
            for (auto k = r.Count(64); k--;)
                t.sort.push_back(r.Pod<SORTCOLUMN>());
            for (auto k = r.Count(128); k--;)
                t.columns.push_back(r.Pod<Column>());
            t.displayName = r.Text();
            p.tabs.push_back(t);
        }
    }
    m.singleExpandedGroup = r.Pod<bool>();
    for (auto &pane : m.panes)
        for (auto &g : pane.groups)
            g.lastSelected = r.Pod<Id>();
    m.windowDpi = r.Pod<UINT>();
    m.groupDoubleClick = r.Pod<bool>();
    if (m.windowDpi && (m.windowDpi < 48 || m.windowDpi > 960))
        throw std::runtime_error("窗口 DPI 无效");
    if (r.pos != b.size() || history.pos != historyBytes.size() ||
        historyCount != m.panes[0].tabs.size() + m.panes[1].tabs.size() || !m.Valid())
        throw std::runtime_error("状态不合法");
    for (const auto &key : m.recent)
        if (!m.locations.contains(key))
            throw std::runtime_error("最近关闭位置缺失");
    for (const auto &pane : m.panes)
        for (const auto &tab : pane.tabs) {
            if (!m.locations.contains(tab.path) || (!tab.home.empty() && !m.locations.contains(tab.home)))
                throw std::runtime_error("标签位置缺失");
            for (const auto &key : tab.history)
                if (!m.locations.contains(key))
                    throw std::runtime_error("历史位置缺失");
        }
    for (int p = 0; p < 2; ++p)
        m.NormalizeGroups(p);
    return m;
}
bool LoadModel(Model &m, const std::filesystem::path &p, bool &recovered, bool &future, bool *obsolete) {
    recovered = future = false;
    if (obsolete)
        *obsolete = false;
    try {
        uint32_t version = 0;
        auto bytes = ReadEnvelope(p, Magic, SessionVersion, &version);
        m = Decode(bytes, version);
        return true;
    } catch (const ObsoleteSession &) {
        if (obsolete)
            *obsolete = true;
        return false;
    } catch (const std::domain_error &) {
        future = true;
        return false;
    } catch (...) {
    }
    auto bak = p;
    bak += L".bak";
    try {
        uint32_t version = 0;
        auto bytes = ReadEnvelope(bak, Magic, SessionVersion, &version);
        m = Decode(bytes, version);
        recovered = true;
        return true;
    } catch (const ObsoleteSession &) {
        if (obsolete)
            *obsolete = true;
        return false;
    } catch (const std::domain_error &) {
        future = true;
        return false;
    } catch (...) {
        return false;
    }
}
} // namespace zen
