#pragma once
#include "platform.h"
#include <unordered_map>
#include <set>
#include <compare>
namespace zen {
// 位置键可以来自用户输入或会话，但不能隐式转换成文件路径或显示文字。
class LocationKey {
    std::wstring value;

  public:
    LocationKey() = default;
    LocationKey(const wchar_t *text) : value(text ? text : L"") {}
    LocationKey(std::wstring text) : value(std::move(text)) {}
    const std::wstring &Stored() const {
        return value;
    }
    bool empty() const {
        return value.empty();
    }
    void clear() {
        value.clear();
    }
    auto operator<=>(const LocationKey &) const = default;
};
struct ShellLocation {
    std::wstring parsingName, displayName;
    std::vector<BYTE> pidl;
    bool usable = true; // 仅运行期有效性，不写入会话。
};
struct LocationResolution {
    std::vector<BYTE> pidl;
    bool savedIdentity = false;
};
struct LocationKeyHash {
    size_t operator()(const LocationKey &key) const {
        return std::hash<std::wstring>{}(key.Stored());
    }
};
class LocationStore {
    std::unordered_map<LocationKey, ShellLocation, LocationKeyHash> entries;

  public:
    const ShellLocation *Find(const LocationKey &key) const;
    // 存储层及测试可导入记录；解析前仍校验 PIDL，不能把内部键交给 Shell 解析。
    bool Insert(const LocationKey &key, ShellLocation location);
    void Set(const LocationKey &key, ShellLocation location);
    LocationKey Remember(IShellItem *item);
    HRESULT Resolve(const LocationKey &key, LocationResolution &result) const;
    HRESULT ResolveItem(const LocationKey &key, IShellItem **item) const;
    PCIDLIST_ABSOLUTE Pidl(const LocationKey &key) const;
    bool Same(const LocationKey &key, PCIDLIST_ABSOLUTE pidl) const;
    std::wstring Text(const LocationKey &key) const;
    std::wstring FilePath(const LocationKey &key) const;
    std::vector<std::pair<std::wstring, LocationKey>> FallbackParts(const LocationKey &key) const;
    void Invalidate(const LocationKey &key);
    void AllowRetry(const LocationKey &key);
    void Retain(const std::set<LocationKey> &keys);
};
} // namespace zen
