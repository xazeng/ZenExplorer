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
    std::wstring id;    // 持久化目录身份，独立于路径文字和显示名称。
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
    struct Presentation {
        std::wstring displayPath, fileSystemPath;
        bool fileSystemKnown = false;
        LocationKey parent;
        std::vector<std::pair<std::wstring, LocationKey>> parts;
    };
    std::unordered_map<LocationKey, Presentation, LocationKeyHash> presentation;

  public:
    std::set<LocationKey> References(const std::set<LocationKey> &roots) const;
    std::wstring EnsureId(const LocationKey &key);
    LocationKey KeyForId(const std::wstring &id);
    size_t Count() const {
        return entries.size();
    }
    void WriteRecords(struct Writer &writer, const std::set<LocationKey> &keys) const;
    LocationKey Import(const LocationStore &source, const LocationKey &key);
    void ReadRecords(struct Reader &reader);
    const ShellLocation *Find(const LocationKey &key) const;
    // 存储层及测试可导入记录；解析前仍校验 PIDL，不能把内部键交给 Shell 解析。
    bool Insert(const LocationKey &key, ShellLocation location);
    void Set(const LocationKey &key, ShellLocation location);
    LocationKey Remember(IShellItem *item);
    // 仅导航完成或显式刷新时调用；其余显示接口只读取缓存，不访问设备或网络。
    LocationKey Refresh(IShellItem *item);
    HRESULT Resolve(const LocationKey &key, LocationResolution &result) const;
    HRESULT ResolveItem(const LocationKey &key, IShellItem **item) const;
    // 编辑文字不是位置键；先使用 Shell 解析，再从已知可读层级按对象身份定位。
    HRESULT ResolveInput(const std::wstring &text, LocationKey &destination);
    HRESULT ValidateDirectory(const LocationKey &key) const;
    PCIDLIST_ABSOLUTE Pidl(const LocationKey &key) const;
    bool Same(const LocationKey &key, PCIDLIST_ABSOLUTE pidl) const;
    std::wstring DisplayName(const LocationKey &key) const;
    std::wstring DisplayPath(const LocationKey &key) const;
    std::wstring EditText(const LocationKey &key) const;
    std::wstring FileSystemPath(const LocationKey &key) const;
    std::vector<std::pair<std::wstring, LocationKey>> Breadcrumbs(const LocationKey &key) const;
    LocationKey Parent(const LocationKey &key) const;
    void Invalidate(const LocationKey &key);
    void AllowRetry(const LocationKey &key);
    void Retain(const std::set<LocationKey> &keys);
};
} // namespace zen
