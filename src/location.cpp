#include "location.h"
#include "storage.h"
#include <map>
namespace zen {
const ShellLocation *LocationStore::Find(const LocationKey &key) const {
    auto it = entries.find(key);
    return it == entries.end() ? nullptr : &it->second;
}
LocationKey LocationStore::Import(const LocationStore &source, const LocationKey &key) {
    auto keys = source.References({key});
    std::map<LocationKey, LocationKey> mapped;
    for (const auto &old : keys) {
        const auto *record = source.Find(old);
        if (!record)
            continue;
        LocationKey target = old;
        bool matched = false;
        for (const auto &[existingKey, existing] : entries)
            if (!record->id.empty() && existing.id == record->id) {
                target = existingKey;
                matched = true;
                break;
            }
        if (!matched) {
            if (entries.contains(target) && entries.at(target).id != record->id)
                target = L"location:" + record->id;
            entries[target] = *record;
        }
        mapped.emplace(old, target);
    }
    for (const auto &[old, target] : mapped)
        if (auto found = source.presentation.find(old); found != source.presentation.end()) {
            auto value = found->second;
            value.parent = mapped.contains(value.parent) ? mapped.at(value.parent) : LocationKey{};
            for (auto &[label, part] : value.parts)
                if (mapped.contains(part))
                    part = mapped.at(part);
            if (!presentation.contains(target))
                presentation[target] = std::move(value);
        }
    return mapped.contains(key) ? mapped.at(key) : LocationKey{};
}
bool LocationStore::Insert(const LocationKey &key, ShellLocation location) {
    return entries.emplace(key, std::move(location)).second;
}
void LocationStore::Set(const LocationKey &key, ShellLocation location) {
    if (location.id.empty())
        if (auto old = Find(key))
            location.id = old->id;
    entries[key] = std::move(location);
}
std::wstring LocationStore::EnsureId(const LocationKey &key) {
    if (key.empty())
        return {};
    if (!entries.contains(key)) {
        ShellLocation value;
        if (!key.Stored().starts_with(L"zen-shell:") && !key.Stored().starts_with(L"location:"))
            value.parsingName = key.Stored();
        value.displayName = LocationName(key.Stored());
        entries.emplace(key, std::move(value));
    }
    auto &value = entries.at(key);
    if (value.id.empty()) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid)))
            throw std::runtime_error("生成目录 ID 失败");
        wchar_t id[40]{};
        StringFromGUID2(guid, id, 40);
        value.id = id;
    }
    return value.id;
}
LocationKey LocationStore::KeyForId(const std::wstring &id) {
    if (id.empty())
        return {};
    GUID guid{};
    if (FAILED(CLSIDFromString(id.c_str(), &guid)))
        throw std::runtime_error("目录 ID 无效");
    for (const auto &[key, value] : entries)
        if (value.id == id)
            return key;
    // 位置资料不可用时仍保留引用，不能把标签重置成其他目录。
    LocationKey key = L"location:" + id;
    ShellLocation missing;
    missing.id = id;
    missing.displayName = L"目录资料不可用";
    missing.usable = false;
    entries[key] = std::move(missing);
    return key;
}
std::set<LocationKey> LocationStore::References(const std::set<LocationKey> &roots) const {
    auto retained = roots;
    std::vector<LocationKey> pending(roots.begin(), roots.end());
    auto add = [&](const LocationKey &key) {
        if (!key.empty() && retained.insert(key).second)
            pending.push_back(key);
    };
    for (size_t i = 0; i < pending.size(); ++i)
        if (auto found = presentation.find(pending[i]); found != presentation.end()) {
            add(found->second.parent);
            for (const auto &[name, key] : found->second.parts)
                add(key);
        }
    retained.erase(LocationKey{});
    return retained;
}
bool LocationStore::Same(const LocationKey &key, PCIDLIST_ABSOLUTE pidl) const {
    auto *location = Find(key);
    return pidl && location && ValidPidl(location->pidl) &&
           ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(location->pidl.data()), pidl);
}
HRESULT LocationStore::ResolveItem(const LocationKey &key, IShellItem **item) const {
    if (!item)
        return E_POINTER;
    *item = nullptr;
    LocationResolution resolved;
    HRESULT hr = Resolve(key, resolved);
    if (FAILED(hr))
        return hr;
    return SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(resolved.pidl.data()),
                                  IID_PPV_ARGS(item));
}
void LocationStore::AllowRetry(const LocationKey &key) {
    auto it = entries.find(key);
    if (it != entries.end() && it->second.parsingName.empty())
        it->second.usable = true;
}
HRESULT LocationStore::ValidateDirectory(const LocationKey &key) const {
    auto path = FileSystemPath(key);
    if (!path.empty()) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return HRESULT_FROM_WIN32(GetLastError());
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
            return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    }
    ComPtr<IShellItem> item;
    HRESULT hr = ResolveItem(key, &item);
    if (FAILED(hr))
        return hr;
    SFGAOF attributes = 0;
    hr = item->GetAttributes(SFGAO_FOLDER | SFGAO_VALIDATE, &attributes);
    return FAILED(hr) ? hr : (attributes & SFGAO_FOLDER) ? S_OK : HRESULT_FROM_WIN32(ERROR_DIRECTORY);
}
HRESULT LocationStore::ResolveInput(const std::wstring &text, LocationKey &destination) {
    destination.clear();
    if (text.size() > 32768 || text.find(L'\0') != std::wstring::npos)
        return E_INVALIDARG;
    if (text.empty()) {
        destination = Home;
        return S_OK;
    }
    ComPtr<IShellItem> item;
    HRESULT native = SHCreateItemFromParsingName(text.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(native)) {
        // 真正的文件系统地址不按显示名称猜测，保留 Shell 的错误结果。
        if (IsAbsoluteDrivePath(text) || text.starts_with(L"\\\\"))
            return native;
        auto normalized = text;
        std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
        while (!normalized.empty() && normalized.back() == L'\\')
            normalized.pop_back();
        size_t longest = 0;
        std::vector<LocationKey> candidates;
        auto consider = [&](const LocationKey &key, const std::wstring &label) {
            if (label.empty() || label.size() > normalized.size() ||
                _wcsnicmp(normalized.c_str(), label.c_str(), label.size()) ||
                (label.size() < normalized.size() && normalized[label.size()] != L'\\'))
                return;
            if (label.size() > longest) {
                longest = label.size();
                candidates.clear();
            }
            if (label.size() == longest &&
                std::find(candidates.begin(), candidates.end(), key) == candidates.end())
                candidates.push_back(key);
        };
        consider(Home, DisplayName(Home));
        for (const auto &[key, value] : presentation)
            if (entries.contains(key))
                consider(key, value.displayPath);
        if (candidates.empty())
            return native;
        // 同一显示路径的不同对象不能任意选一个；同一 PIDL 的别名允许合并。
        LocationResolution identity;
        HRESULT hr = Resolve(candidates.front(), identity);
        if (FAILED(hr))
            return hr;
        for (size_t i = 1; i < candidates.size(); ++i) {
            LocationResolution other;
            if (FAILED(Resolve(candidates[i], other)) ||
                !ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(identity.pidl.data()),
                           reinterpret_cast<PCIDLIST_ABSOLUTE>(other.pidl.data())))
                return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
        }
        hr = ResolveItem(candidates.front(), &item);
        if (FAILED(hr))
            return hr;
        size_t offset = longest;
        while (offset < normalized.size()) {
            ++offset;
            size_t end = normalized.find(L'\\', offset);
            if (end == std::wstring::npos)
                end = normalized.size();
            auto component = normalized.substr(offset, end - offset);
            if (component.empty())
                return E_INVALIDARG;
            std::vector<ComPtr<IShellItem>> folders;
            hr = ChildFolderItems(item.Get(), folders);
            if (FAILED(hr))
                return hr;
            ComPtr<IShellItem> match;
            for (const auto &folder : folders) {
                auto name = ItemName(folder.Get(), SIGDN_NORMALDISPLAY);
                auto addressName = ItemName(folder.Get(), SIGDN_PARENTRELATIVEFORADDRESSBAR);
                if (_wcsicmp(component.c_str(), name.c_str()) &&
                    _wcsicmp(component.c_str(), addressName.c_str()))
                    continue;
                if (match)
                    return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
                match = folder;
            }
            if (!match)
                return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
            item = std::move(match);
            offset = end;
        }
    }
    SFGAOF attributes = 0;
    HRESULT hr = item->GetAttributes(SFGAO_FOLDER, &attributes);
    if (FAILED(hr))
        return hr;
    if (!(attributes & SFGAO_FOLDER))
        return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    destination = Remember(item.Get());
    return destination.empty() ? E_FAIL : S_OK;
}
void LocationStore::Retain(const std::set<LocationKey> &keys) {
    auto retained = References(keys);
    std::erase_if(entries, [&](const auto &entry) { return !retained.contains(entry.first); });
    std::erase_if(presentation, [&](const auto &entry) { return !retained.contains(entry.first); });
}
LocationKey LocationStore::Refresh(IShellItem *item) {
    auto key = Remember(item);
    if (key.empty())
        return {};
    SFGAOF attributes = 0;
    const bool known = SUCCEEDED(item->GetAttributes(SFGAO_FILESYSTEM, &attributes));
    const auto file =
        known && (attributes & SFGAO_FILESYSTEM) ? ItemName(item, SIGDN_FILESYSPATH) : std::wstring{};
    std::vector<ComPtr<IShellItem>> objects;
    HRESULT hr = ShellHierarchy(item, objects);
    if (FAILED(hr))
        Log(L"位置显示层级不完整 " + ErrorText(hr));
    Presentation next;
    next.fileSystemKnown = known;
    next.fileSystemPath = file;
    std::vector<std::wstring> names;
    for (size_t i = 0; i < objects.size(); ++i) {
        auto target = Remember(objects[i].Get());
        if (target.empty())
            break;
        if (i == 1)
            next.parent = target;
        bool drive = target.Stored().size() == 3 && IsAbsoluteDrivePath(target.Stored());
        auto label =
            ItemName(objects[i].Get(), drive ? SIGDN_NORMALDISPLAY : SIGDN_PARENTRELATIVEFORADDRESSBAR);
        if (label.empty())
            label = DisplayName(target);
        next.parts.push_back({label, target});
        names.push_back(DisplayName(target));
    }
    std::reverse(next.parts.begin(), next.parts.end());
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        if (!next.displayPath.empty())
            next.displayPath += L"\\";
        next.displayPath += *it;
    }
    if (SUCCEEDED(hr) && file.empty()) {
        std::wstring prefix;
        for (size_t i = 0; i < next.parts.size(); ++i) {
            if (!prefix.empty())
                prefix += L"\\";
            prefix += names[names.size() - 1 - i];
            const auto &target = next.parts[i].second;
            // 已取得的父级资料也可用于最近关闭记录，不再逐项访问设备。
            if (FileSystemPath(target).empty())
                presentation[target].displayPath = prefix;
        }
    }
    if (!file.empty()) {
        std::vector<std::pair<std::wstring, LocationKey>> parts;
        for (const auto &[label, target] : zen::Breadcrumbs(file))
            parts.push_back({label, target});
        for (auto &[label, target] : parts)
            for (const auto &[shellLabel, shellTarget] : next.parts)
                if (!_wcsicmp(target.Stored().c_str(), shellTarget.Stored().c_str())) {
                    label = shellLabel;
                    target = shellTarget;
                    break;
                }
        next.parts = std::move(parts);
        next.displayPath = file;
    }
    if (next.displayPath.empty())
        next.displayPath = DisplayName(key);
    if (next.parts.empty())
        next.parts = {{DisplayName(key), key}};
    // 不用一次失败的层级查询覆盖已保存的完整显示路径。
    if (FAILED(hr) && file.empty())
        if (auto old = presentation.find(key); old != presentation.end() && !old->second.displayPath.empty())
            next.displayPath = old->second.displayPath;
    presentation[key] = std::move(next);
    return key;
}
LocationKey LocationStore::Remember(IShellItem *item) {
    if (!item)
        return {};
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHGetIDListFromObject(item, &pidl);
    if (FAILED(hr) || !pidl) {
        Log(L"无法获取目录的 Shell 标识 " + ErrorText(FAILED(hr) ? hr : E_FAIL));
        return {};
    }
    ShellLocation target;
    target.parsingName = ItemName(item);
    target.displayName = ItemName(item, SIGDN_NORMALDISPLAY);
    auto key = target.parsingName;
    target.pidl.assign((BYTE *)pidl, (BYTE *)pidl + ILGetSize(pidl));
    CoTaskMemFree(pidl);
    auto existing = entries.find(key);
    bool collision = existing != entries.end() && ValidPidl(existing->second.pidl) &&
                     existing->second.usable &&
                     !ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(existing->second.pidl.data()),
                                reinterpret_cast<PCIDLIST_ABSOLUTE>(target.pidl.data()));
    if (key.empty() || collision) {
        // 名称缺失或不同对象返回同名时，以真实标识区分，避免父子层级互相覆盖。
        key = L"zen-shell:";
        constexpr wchar_t digits[] = L"0123456789abcdef";
        for (BYTE b : target.pidl) {
            key += digits[b >> 4];
            key += digits[b & 15];
        }
    }
    Set(key, std::move(target));
    return key;
}
PCIDLIST_ABSOLUTE LocationStore::Pidl(const LocationKey &key) const {
    auto location = entries.find(key);
    return location != entries.end() && location->second.usable && ValidPidl(location->second.pidl)
               ? reinterpret_cast<PCIDLIST_ABSOLUTE>(location->second.pidl.data())
               : nullptr;
}
HRESULT LocationStore::Resolve(const LocationKey &key, LocationResolution &result) const {
    auto &pidl = result.pidl;
    auto &savedIdentity = result.savedIdentity;
    pidl.clear();
    savedIdentity = false;
    if (key.empty())
        return E_INVALIDARG;
    if (auto saved = Pidl(key)) {
        pidl.assign(reinterpret_cast<const BYTE *>(saved),
                    reinterpret_cast<const BYTE *>(saved) + ILGetSize(saved));
        savedIdentity = true;
        return S_OK;
    }
    auto location = entries.find(key);
    auto name = location == entries.end() ? key.Stored() : location->second.parsingName;
    if (name.empty() || name.starts_with(L"zen-shell:"))
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    PIDLIST_ABSOLUTE parsed = nullptr;
    HRESULT hr = SHParseDisplayName(name.c_str(), nullptr, &parsed, 0, nullptr);
    if (SUCCEEDED(hr) && parsed)
        pidl.assign(reinterpret_cast<const BYTE *>(parsed),
                    reinterpret_cast<const BYTE *>(parsed) + ILGetSize(parsed));
    CoTaskMemFree(parsed);
    return SUCCEEDED(hr) && !ValidPidl(pidl) ? E_FAIL : hr;
}
void LocationStore::Invalidate(const LocationKey &path) {
    auto found = entries.find(path);
    if (found == entries.end() || !ValidPidl(found->second.pidl))
        return;
    auto invalid = found->second.pidl;
    auto matches = [&](const std::vector<BYTE> &bytes) {
        if (!ValidPidl(bytes))
            return false;
        auto parent = reinterpret_cast<PCIDLIST_ABSOLUTE>(invalid.data());
        auto child = reinterpret_cast<PCIDLIST_ABSOLUTE>(bytes.data());
        return ILIsEqual(parent, child) || ILIsParent(parent, child, FALSE);
    };
    for (auto &[key, location] : entries)
        if (matches(location.pidl))
            location.usable = false;
    Log(L"导航失败，目标及下级标识已标记失效");
}
std::wstring LocationStore::DisplayPath(const LocationKey &key) const {
    const auto &text = key.Stored();
    if (key == Home)
        return L"此电脑";
    if (auto it = presentation.find(key); it != presentation.end() && !it->second.displayPath.empty())
        return it->second.displayPath;
    // 已有完整磁盘/UNC 地址保留全路径；尚未加载的虚拟位置使用保存的可读名称。
    if (IsAbsoluteDrivePath(text) || text.starts_with(L"\\\\"))
        return text;
    return DisplayName(key);
}
std::wstring LocationStore::DisplayName(const LocationKey &key) const {
    if (key == Home)
        return L"此电脑";
    auto found = entries.find(key);
    if (found != entries.end() && !found->second.displayName.empty())
        return found->second.displayName;
    auto name = LocationName(key.Stored());
    return name.starts_with(L"::") || name.starts_with(L"zen-shell:") ? L"未知位置" : name;
}
std::wstring LocationStore::EditText(const LocationKey &key) const {
    return key == Home ? std::wstring{} : DisplayPath(key);
}
LocationKey LocationStore::Parent(const LocationKey &key) const {
    auto found = presentation.find(key);
    return found != presentation.end() ? found->second.parent : LocationKey{};
}
std::wstring LocationStore::FileSystemPath(const LocationKey &key) const {
    if (auto it = presentation.find(key); it != presentation.end() && it->second.fileSystemKnown)
        return it->second.fileSystemPath;
    auto *location = Find(key);
    const auto &path = location ? location->parsingName : key.Stored();
    // 这里只取得已有文件路径，供本地图标等无阻塞检查使用；不解析设备或网络对象。
    return IsAbsoluteDrivePath(path) || path.starts_with(L"\\\\") ? path : std::wstring{};
}
std::vector<std::pair<std::wstring, LocationKey>> LocationStore::Breadcrumbs(const LocationKey &key) const {
    if (auto it = presentation.find(key); it != presentation.end() && !it->second.parts.empty())
        return it->second.parts;
    auto path = FileSystemPath(key);
    if (path.empty())
        return {{DisplayName(key), key}};
    std::vector<std::pair<std::wstring, LocationKey>> result;
    for (const auto &[name, target] : zen::Breadcrumbs(path))
        result.push_back({name, target});
    return result;
}
void LocationStore::WriteRecords(Writer &w, const std::set<LocationKey> &keys) const {
    if (keys.size() > 1048576)
        throw std::runtime_error("目录资料过多");
    auto reference = [&](const LocationKey &key) {
        if (key.empty()) {
            w.Text(L"");
            return;
        }
        auto value = Find(key);
        if (!value || value->id.empty())
            throw std::runtime_error("目录引用缺失");
        w.Text(value->id);
    };
    w.Pod((uint32_t)keys.size());
    for (const auto &key : keys) {
        auto *value = Find(key);
        if (!value || value->id.empty() || (!value->pidl.empty() && !ValidPidl(value->pidl)))
            throw std::runtime_error("目录资料无效");
        if (key.Stored().size() > 32768 || value->parsingName.size() > 32768 ||
            value->displayName.size() > 32768)
            throw std::runtime_error("目录文字过长");
        reference(key);
        w.Text(key.Stored());
        w.Text(value->parsingName);
        w.Text(value->displayName);
        w.Pod((uint32_t)value->pidl.size());
        w.bytes.insert(w.bytes.end(), value->pidl.begin(), value->pidl.end());
        Presentation empty;
        auto found = presentation.find(key);
        const auto &display = found == presentation.end() ? empty : found->second;
        if (display.displayPath.size() > 32768 || display.fileSystemPath.size() > 32768 ||
            display.parts.size() > 256)
            throw std::runtime_error("目录显示资料过大");
        w.Text(display.displayPath);
        w.Pod(display.fileSystemKnown);
        w.Text(display.fileSystemPath);
        reference(display.parent);
        w.Pod((uint32_t)display.parts.size());
        for (const auto &[label, target] : display.parts) {
            if (label.size() > 32768)
                throw std::runtime_error("层级名称过长");
            w.Text(label);
            reference(target);
        }
    }
}
void LocationStore::ReadRecords(Reader &r) {
    struct Links {
        LocationKey key;
        std::wstring parent;
        std::vector<std::pair<std::wstring, std::wstring>> parts;
    };
    LocationStore next;
    std::vector<Links> links;
    std::set<std::wstring> ids;
    for (auto n = r.Count(1048576); n--;) {
        ShellLocation value;
        value.id = r.Text();
        GUID guid{};
        if (FAILED(CLSIDFromString(value.id.c_str(), &guid)) || !ids.insert(value.id).second)
            throw std::runtime_error("目录 ID 无效或重复");
        LocationKey key = r.Text();
        value.parsingName = r.Text();
        value.displayName = r.Text();
        auto count = r.Count(1024 * 1024);
        if (count > r.bytes.size() - r.pos)
            throw std::runtime_error("目录标识截断");
        value.pidl.assign(r.bytes.begin() + r.pos, r.bytes.begin() + r.pos + count);
        r.pos += count;
        if (key.empty() || (!value.pidl.empty() && !ValidPidl(value.pidl)) || !next.Insert(key, value))
            throw std::runtime_error("目录资料无效");
        auto &display = next.presentation[key];
        display.displayPath = r.Text();
        display.fileSystemKnown = r.Pod<bool>();
        display.fileSystemPath = r.Text();
        Links record{key, r.Text(), {}};
        for (auto size = r.Count(256); size--;) {
            auto label = r.Text();
            auto target = r.Text();
            record.parts.push_back({label, target});
        }
        links.push_back(std::move(record));
    }
    for (const auto &record : links) {
        auto &display = next.presentation.at(record.key);
        if (!record.parent.empty() && !ids.contains(record.parent))
            throw std::runtime_error("父级目录引用缺失");
        display.parent = next.KeyForId(record.parent);
        for (const auto &[label, target] : record.parts) {
            if (!ids.contains(target))
                throw std::runtime_error("层级目录引用缺失");
            display.parts.push_back({label, next.KeyForId(target)});
        }
    }
    *this = std::move(next);
}
} // namespace zen
