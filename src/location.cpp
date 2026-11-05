#include "location.h"
namespace zen {
const ShellLocation *LocationStore::Find(const LocationKey &key) const {
    auto it = entries.find(key);
    return it == entries.end() ? nullptr : &it->second;
}
bool LocationStore::Insert(const LocationKey &key, ShellLocation location) {
    return entries.emplace(key, std::move(location)).second;
}
void LocationStore::Set(const LocationKey &key, ShellLocation location) {
    entries[key] = std::move(location);
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
void LocationStore::Retain(const std::set<LocationKey> &keys) {
    std::erase_if(entries, [&](const auto &entry) { return !keys.contains(entry.first); });
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
    entries[key] = std::move(target);
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
std::wstring LocationStore::Text(const LocationKey &key) const {
    const auto &text = key.Stored();
    if (key == Home)
        return L"此电脑";
    // 已有完整磁盘/UNC 地址保留全路径；尚未加载的虚拟位置使用保存的可读名称。
    if ((text.size() > 2 && text[1] == L':') || text.starts_with(L"\\\\"))
        return text;
    auto found = entries.find(key);
    return found != entries.end() && !found->second.displayName.empty() ? found->second.displayName
                                                                        : LocationName(text);
}
std::wstring LocationStore::FilePath(const LocationKey &key) const {
    auto *location = Find(key);
    const auto &path = location ? location->parsingName : key.Stored();
    // 这里只取得已有文件路径，供本地图标等无阻塞检查使用；不解析设备或网络对象。
    return (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
                   path.starts_with(L"\\\\")
               ? path
               : std::wstring{};
}
std::vector<std::pair<std::wstring, LocationKey>> LocationStore::FallbackParts(const LocationKey &key) const {
    auto path = FilePath(key);
    if (path.empty())
        return {{Text(key), key}};
    std::vector<std::pair<std::wstring, LocationKey>> result;
    for (const auto &[name, target] : Breadcrumbs(path))
        result.push_back({name, target});
    return result;
}
} // namespace zen
