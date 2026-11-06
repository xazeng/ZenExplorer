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
static constexpr uint32_t Magic = 0x434E455A, HistoryMagic = 0x484E455A, TabsMagic = 0x544E455A,
                          LocationsMagic = 0x4C4E455A;
static constexpr uint32_t SessionVersion = 7, HistoryVersion = 3, TabsVersion = 2;
std::filesystem::path TabsPath(const std::filesystem::path &session) {
    return session.parent_path() / (session.filename() == L"session.dat"
                                        ? std::filesystem::path(L"tabs.dat")
                                        : std::filesystem::path(session.stem().wstring() + L".tabs.dat"));
}
static std::filesystem::path HistoryPath(const std::filesystem::path &session, const GUID &generation) {
    wchar_t name[40]{};
    StringFromGUID2(generation, name, 40);
    return DataDir() / L"history" / session.stem() / (std::wstring(name) + L".dat");
}
std::filesystem::path LocationsPath(const std::filesystem::path &session, const GUID &generation) {
    return DataDir() / L"locations" / session.stem() / HistoryPath(session, generation).filename();
}
static std::vector<uint8_t> ReadCurrent(const std::filesystem::path &p, uint32_t magic, uint32_t version) {
    uint32_t actual = 0;
    auto bytes = ReadEnvelope(p, magic, version, &actual);
    if (actual != version)
        throw std::runtime_error("格式版本不匹配");
    return bytes;
}
static Model DecodeTabs(const std::vector<uint8_t> &bytes, GUID &generation,
                        const LocationStore &locations = {}) {
    Reader r{bytes};
    generation = r.Pod<GUID>();
    Model m;
    m.locations = locations;
    m.nextId = r.Pod<Id>();
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
            g.lastSelected = r.Pod<Id>();
            p.groups.push_back(g);
        }
        for (auto n = r.Count(); n--;) {
            Tab t;
            t.id = r.Pod<Id>();
            t.group = r.Pod<Id>();
            t.path = m.locations.KeyForId(r.Text());
            t.name = r.Text();
            t.home = m.locations.KeyForId(r.Text());
            t.displayName = m.locations.DisplayName(t.path);
            t.history = {t.path};
            p.tabs.push_back(std::move(t));
        }
    }
    if (r.pos != bytes.size() || !m.Valid())
        throw std::runtime_error("标签结构无效");
    return m;
}
static LocationStore RecoverLocations(const std::filesystem::path &session) {
    LocationStore result;
    try {
        auto backup = TabsPath(session);
        backup += L".bak";
        auto bytes = ReadCurrent(backup, TabsMagic, TabsVersion);
        GUID generation{};
        DecodeTabs(bytes, generation);
        auto records = ReadCurrent(LocationsPath(session, generation), LocationsMagic, 1);
        Reader reader{records};
        result.ReadRecords(reader);
        if (reader.pos != records.size())
            return {};
    } catch (...) {
    }
    return result;
}
static void CleanupSnapshots(const std::filesystem::path &session) {
    try {
        std::set<std::filesystem::path> retained;
        auto path = TabsPath(session);
        for (int i = 0; i < 2; ++i) {
            if (std::filesystem::exists(path)) {
                GUID generation{};
                DecodeTabs(ReadCurrent(path, TabsMagic, TabsVersion), generation);
                retained.insert(LocationsPath(session, generation).filename());
            }
            path += L".bak";
        }
        // 仅扫描当前会话专属目录，只清理本格式、合法 GUID 命名且无引用的快照。
        for (const auto &[folder, magic, version] :
             {std::tuple{DataDir() / L"locations" / session.stem(), LocationsMagic, 1u},
              std::tuple{DataDir() / L"history" / session.stem(), HistoryMagic, HistoryVersion}}) {
            if (!std::filesystem::exists(folder))
                continue;
            for (const auto &entry : std::filesystem::directory_iterator(folder)) {
                auto file = entry.path();
                GUID parsed{};
                if (!entry.is_regular_file() || file.extension() != L".dat" ||
                    retained.contains(file.filename()) ||
                    FAILED(CLSIDFromString(file.stem().c_str(), &parsed)))
                    continue;
                try {
                    ReadCurrent(file, magic, version);
                    std::error_code ec;
                    std::filesystem::remove(file, ec);
                } catch (...) {
                    // 未知版本及无法识别的文件不是当前清理对象。
                }
            }
        }
    } catch (...) {
        Log(L"目录快照清理跳过，保留已有数据");
    }
}
// 会话仅保存可丢弃的界面状态；必须匹配标签快照，不能把旧设置套到新标签上。
static Model DecodeSession(const std::vector<uint8_t> &bytes, const Model &tabs, const GUID &generation) {
    Reader r{bytes};
    if (!IsEqualGUID(r.Pod<GUID>(), generation))
        throw std::runtime_error("会话与标签快照不匹配");
    Model m = tabs;
    m.active = r.Pod<int>();
    m.split = r.Pod<double>();
    m.wrap = r.Pod<bool>();
    m.window = r.Pod<RECT>();
    m.maximized = r.Pod<bool>();
    for (auto &pane : m.panes) {
        if (r.Count() != pane.tabs.size())
            throw std::runtime_error("会话标签数量不匹配");
        for (auto &t : pane.tabs) {
            if (r.Pod<Id>() != t.id)
                throw std::runtime_error("会话标签不匹配");
            t.viewMode = r.Pod<UINT>();
            t.iconSize = r.Pod<int>();
            for (auto n = r.Count(64); n--;)
                t.sort.push_back(r.Pod<SORTCOLUMN>());
            for (auto n = r.Count(128); n--;)
                t.columns.push_back(r.Pod<Column>());
        }
    }
    m.singleExpandedGroup = r.Pod<bool>();
    m.windowDpi = r.Pod<UINT>();
    m.groupDoubleClick = r.Pod<bool>();
    if (r.pos != bytes.size() || !m.Valid())
        throw std::runtime_error("会话状态无效");
    return m;
}
static void DecodeHistory(Model &m, const std::vector<uint8_t> &bytes) {
    Reader r{bytes};
    for (auto n = r.Count(10); n--;) {
        auto key = m.locations.KeyForId(r.Text());
        if (key.empty())
            throw std::runtime_error("最近关闭目录 ID 为空");
        m.recent.push_back(key);
    }
    if (r.Count(8192) != m.panes[0].tabs.size() + m.panes[1].tabs.size())
        throw std::runtime_error("历史标签数量不匹配");
    for (auto &pane : m.panes)
        for (auto &t : pane.tabs) {
            if (r.Pod<Id>() != t.id)
                throw std::runtime_error("历史标签不匹配");
            t.historyIndex = r.Pod<int>();
            t.history.clear();
            for (auto n = r.Count(128); n--;) {
                auto key = m.locations.KeyForId(r.Text());
                if (key.empty())
                    throw std::runtime_error("历史目录 ID 为空");
                t.history.push_back(key);
            }
        }
    if (r.pos != bytes.size() || !m.Valid())
        throw std::runtime_error("历史状态无效");
}
static bool WriteSnapshot(const std::filesystem::path &p, uint32_t magic, const Writer &w, uint32_t version) {
    if (w.bytes.size() > 32 * 1024 * 1024)
        return false;
    // 只有通过格式及校验检查的主文件才可替换有效备份。
    bool backup = false;
    try {
        auto bytes = ReadCurrent(p, magic, version);
        if (magic == TabsMagic) {
            GUID ignored{};
            DecodeTabs(bytes, ignored);
        }
        backup = true;
    } catch (const std::domain_error &) {
        return false;
    } catch (...) {
    }
    return WriteEnvelope(p, magic, w.bytes, backup, version);
}
bool SaveModel(Model &m, const std::filesystem::path &p) {
    if (!m.Valid())
        return false;
    try {
        // 在写入任何文件前检查未来版本，拒绝部分替换已有新格式配置。
        for (const auto &[path, magic, version] :
             {std::tuple{p, Magic, SessionVersion}, std::tuple{TabsPath(p), TabsMagic, TabsVersion}}) {
            try {
                ReadCurrent(path, magic, version);
            } catch (const std::domain_error &) {
                return false;
            } catch (...) {
            }
        }
        GUID generation{};
        if (FAILED(CoCreateGuid(&generation)))
            return false;
        Writer tabs, session, history, locations;
        tabs.Pod(generation);
        session.Pod(generation);
        std::set<LocationKey> coreKeys, historyKeys(m.recent.begin(), m.recent.end());
        for (const auto &entry : m.quickAccess.links)
            coreKeys.insert(entry.target);
        for (const auto &pane : m.panes)
            for (const auto &t : pane.tabs) {
                coreKeys.insert(t.path);
                if (!t.home.empty())
                    coreKeys.insert(t.home);
                historyKeys.insert(t.history.begin(), t.history.end());
            }
        coreKeys.insert(historyKeys.begin(), historyKeys.end());
        auto retained = m.locations.References(coreKeys);
        for (const auto &key : retained)
            m.locations.EnsureId(key);
        for (const auto &pane : m.panes)
            for (const auto &tab : pane.tabs)
                if (!tab.displayName.empty()) {
                    auto record = *m.locations.Find(tab.path);
                    record.displayName = tab.displayName;
                    m.locations.Set(tab.path, std::move(record));
                }
        m.locations.WriteRecords(locations, retained);
        history.Pod((uint32_t)m.recent.size());
        for (const auto &key : m.recent)
            history.Text(m.locations.EnsureId(key));
        history.Pod((uint32_t)(m.panes[0].tabs.size() + m.panes[1].tabs.size()));
        tabs.Pod(m.nextId);
        session.Pod(m.active);
        session.Pod(m.split);
        session.Pod(m.wrap);
        session.Pod(m.window);
        session.Pod(m.maximized);
        for (const auto &pane : m.panes) {
            tabs.Pod(pane.selected);
            tabs.Pod((uint32_t)pane.groups.size());
            for (const auto &g : pane.groups) {
                tabs.Pod(g.id);
                tabs.Text(g.name);
                tabs.Pod(g.color);
                tabs.Pod(g.collapsed);
                tabs.Pod(g.lastSelected);
            }
            tabs.Pod((uint32_t)pane.tabs.size());
            session.Pod((uint32_t)pane.tabs.size());
            for (const auto &t : pane.tabs) {
                tabs.Pod(t.id);
                tabs.Pod(t.group);
                tabs.Text(m.locations.EnsureId(t.path));
                tabs.Text(t.name);
                tabs.Text(m.locations.EnsureId(t.home));
                session.Pod(t.id);
                session.Pod(t.viewMode);
                session.Pod(t.iconSize);
                session.Pod((uint32_t)t.sort.size());
                for (const auto &x : t.sort)
                    session.Pod(x);
                session.Pod((uint32_t)t.columns.size());
                for (const auto &x : t.columns)
                    session.Pod(x);
                history.Pod(t.id);
                history.Pod(t.historyIndex);
                history.Pod((uint32_t)t.history.size());
                for (const auto &key : t.history)
                    history.Text(m.locations.EnsureId(key));
            }
        }
        session.Pod(m.singleExpandedGroup);
        session.Pod(m.windowDpi);
        session.Pod(m.groupDoubleClick);
        auto tabPath = TabsPath(p);
        auto backup = tabPath;
        backup += L".bak";
        auto historyPath = HistoryPath(p, generation);
        auto locationPath = LocationsPath(p, generation);
        if (locations.bytes.size() > 32 * 1024 * 1024 || history.bytes.size() > 32 * 1024 * 1024)
            return false;
        if (!WriteEnvelope(locationPath, LocationsMagic, locations.bytes, false) ||
            !WriteEnvelope(historyPath, HistoryMagic, history.bytes, false, HistoryVersion))
            return false;
        // tabs 是提交点；任何附属文件写入失败都不能替换已经保存的标签关系。
        if (!WriteSnapshot(p, Magic, session, SessionVersion) ||
            !WriteSnapshot(tabPath, TabsMagic, tabs, TabsVersion)) {
            std::error_code ec;
            std::filesystem::remove(historyPath, ec);
            std::filesystem::remove(locationPath, ec);
            return false;
        }
        CleanupSnapshots(p);
        return true;
    } catch (...) {
        Log(L"保存独立标签配置失败");
        return false;
    }
}
bool LoadModel(Model &m, const std::filesystem::path &p, bool &recovered, bool &future) {
    recovered = future = false;
    GUID generation{};
    Model tabs;
    bool loaded = false;
    auto path = TabsPath(p);
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            auto bytes = ReadCurrent(path, TabsMagic, TabsVersion);
            Reader header{bytes};
            generation = header.Pod<GUID>();
            LocationStore locations;
            try {
                auto records = ReadCurrent(LocationsPath(p, generation), LocationsMagic, 1);
                Reader input{records};
                locations.ReadRecords(input);
                if (input.pos != records.size())
                    throw std::runtime_error("location trailing data");
            } catch (const std::domain_error &) {
                future = true;
            } catch (...) {
                locations = RecoverLocations(p);
            }
            tabs = DecodeTabs(bytes, generation, locations);
            loaded = true;
            recovered = attempt != 0;
            break;
        } catch (const std::domain_error &) {
            future = true;
            return false;
        } catch (...) {
            path += L".bak";
        }
    }
    if (!loaded) {
        // 已有但无法读取的核心文件不得被默认会话覆盖，保留用户数据待修复。
        std::error_code ec;
        future = std::filesystem::exists(TabsPath(p), ec) ||
                 std::filesystem::exists(TabsPath(p).wstring() + L".bak", ec);
        return false;
    }
    m = tabs;
    bool sessionLoaded = false;
    path = p;
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            m = DecodeSession(ReadCurrent(path, Magic, SessionVersion), tabs, generation);
            sessionLoaded = true;
            break;
        } catch (const std::domain_error &) {
            future = true;
            break;
        } catch (...) {
            path += L".bak";
        }
    }
    if (sessionLoaded) {
        try {
            Model withHistory = m;
            DecodeHistory(withHistory, ReadCurrent(HistoryPath(p, generation), HistoryMagic, HistoryVersion));
            m = std::move(withHistory);
        } catch (...) {
            Log(L"历史不可用，保留独立标签配置");
        }
    } else {
        // 附属配置不能决定标签是否存在；主目录标签回到独立保存的主目录。
        for (auto &pane : m.panes)
            for (auto &t : pane.tabs)
                if (!t.home.empty()) {
                    t.path = t.home;
                    t.history = {t.path};
                    t.displayName = m.locations.DisplayName(t.path);
                }
        Log(L"会话设置不可用，已从独立文件恢复标签与分组");
    }
    for (int pane = 0; pane < 2; ++pane)
        m.NormalizeGroups(pane);
    return true;
}
} // namespace zen
