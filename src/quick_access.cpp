#include "quick_access.h"
#include "storage.h"
namespace zen {
static constexpr uint32_t QuickMagic = 0x514E455A, RecordsMagic = 0x4C4E455A;
static std::filesystem::path RecordsPath(const std::filesystem::path &file, const GUID &generation) {
    wchar_t name[40]{};
    StringFromGUID2(generation, name, 40);
    return DataDir() / L"quick-access" / file.stem() / (std::wstring(name) + L".dat");
}
uint64_t QuickAccess::Put(LocationStore &locations, const LocationKey &target, const std::wstring &name) {
    if (readOnly || target.empty() || name.empty() || name.size() > 32768)
        return 0;
    auto identity = locations.EnsureId(target);
    for (auto &entry : links) {
        auto pidl = locations.Pidl(target);
        if (locations.EnsureId(entry.target) == identity || (pidl && locations.Same(entry.target, pidl))) {
            entry.name = name;
            return entry.id;
        }
    }
    if (links.size() >= 4096 || nextId == UINT64_MAX)
        return 0;
    auto id = nextId++;
    links.push_back({id, name, target});
    return id;
}
bool QuickAccess::Move(size_t index, int direction) {
    if (readOnly || index >= links.size() || (direction != -1 && direction != 1) ||
        (direction == -1 && index == 0) || (direction == 1 && index + 1 == links.size()))
        return false;
    std::swap(links[index], links[index + direction]);
    return true;
}
bool QuickAccess::Remove(size_t index) {
    if (readOnly || index >= links.size())
        return false;
    links.erase(links.begin() + index);
    return true;
}
static QuickAccess Decode(const std::vector<uint8_t> &bytes, LocationStore &locations) {
    Reader r{bytes};
    r.Pod<GUID>();
    QuickAccess result;
    result.nextId = r.Pod<uint64_t>();
    std::set<uint64_t> ids;
    for (auto n = r.Count(); n--;) {
        QuickLink entry;
        entry.id = r.Pod<uint64_t>();
        entry.name = r.Text();
        entry.target = locations.KeyForId(r.Text());
        if (!entry.id || entry.id >= result.nextId || entry.name.empty() || entry.target.empty() ||
            !ids.insert(entry.id).second)
            throw std::runtime_error("快捷访问数据无效");
        result.links.push_back(std::move(entry));
    }
    if (r.pos != bytes.size() || !result.nextId)
        throw std::runtime_error("快捷访问长度无效");
    return result;
}
void QuickAccess::Load(LocationStore &locations, const std::filesystem::path &file) {
    links.clear();
    nextId = 1;
    readOnly = false;
    auto path = file;
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            auto bytes = ReadEnvelope(path, QuickMagic);
            Reader header{bytes};
            auto generation = header.Pod<GUID>();
            LocationStore source;
            try {
                auto records = ReadEnvelope(RecordsPath(file, generation), RecordsMagic);
                Reader input{records};
                source.ReadRecords(input);
                if (input.pos != records.size())
                    throw std::runtime_error("快捷访问目录资料无效");
            } catch (const std::domain_error &) {
                readOnly = true;
            } catch (...) {
                source = {};
            }
            auto restored = Decode(bytes, source);
            restored.readOnly = readOnly;
            for (auto &entry : restored.links)
                entry.target = locations.Import(source, entry.target);
            *this = std::move(restored);
            return;
        } catch (const std::domain_error &) {
            readOnly = true;
            return;
        } catch (...) {
            path += L".bak";
        }
    }
    std::error_code ec;
    readOnly = std::filesystem::exists(file, ec) || std::filesystem::exists(file.wstring() + L".bak", ec);
}
bool QuickAccess::Save(LocationStore &locations, const std::filesystem::path &file) const {
    if (readOnly || links.size() > 4096)
        return false;
    try {
        bool backup = false;
        try {
            auto bytes = ReadEnvelope(file, QuickMagic);
            LocationStore check;
            Decode(bytes, check);
            backup = true;
        } catch (const std::domain_error &) {
            return false;
        } catch (...) {
        }
        std::set<LocationKey> roots;
        for (const auto &entry : links)
            roots.insert(entry.target);
        auto keys = locations.References(roots);
        for (const auto &key : keys)
            locations.EnsureId(key);
        GUID generation{};
        if (FAILED(CoCreateGuid(&generation)))
            return false;
        Writer records, data;
        locations.WriteRecords(records, keys);
        data.Pod(generation);
        data.Pod(nextId);
        data.Pod((uint32_t)links.size());
        for (const auto &entry : links) {
            data.Pod(entry.id);
            data.Text(entry.name);
            data.Text(locations.EnsureId(entry.target));
        }
        LocationStore check;
        Decode(data.bytes, check);
        if (records.bytes.size() > 32 * 1024 * 1024 || data.bytes.size() > 32 * 1024 * 1024)
            return false;
        auto target = RecordsPath(file, generation);
        if (!WriteEnvelope(target, RecordsMagic, records.bytes, false))
            return false;
        if (!WriteEnvelope(file, QuickMagic, data.bytes, backup)) {
            std::error_code ec;
            std::filesystem::remove(target, ec);
            return false;
        }
        // 主文件和有效备份各自持有目录资料；其他快照只在提交成功后清理。
        try {
            std::set<std::filesystem::path> keep{target};
            auto old = file;
            old += L".bak";
            if (std::filesystem::exists(old)) {
                auto bytes = ReadEnvelope(old, QuickMagic);
                Reader r{bytes};
                keep.insert(RecordsPath(file, r.Pod<GUID>()));
            }
            for (const auto &entry : std::filesystem::directory_iterator(target.parent_path())) {
                GUID id{};
                if (entry.is_regular_file() && entry.path().extension() == L".dat" &&
                    SUCCEEDED(CLSIDFromString(entry.path().stem().c_str(), &id)) &&
                    !keep.contains(entry.path())) {
                    try {
                        ReadEnvelope(entry.path(), RecordsMagic);
                        std::error_code ec;
                        std::filesystem::remove(entry.path(), ec);
                    } catch (...) {
                    }
                }
            }
        } catch (...) {
            Log(L"快捷访问旧资料清理跳过");
        }
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace zen
