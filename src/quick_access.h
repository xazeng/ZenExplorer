#pragma once
#include "location.h"
namespace zen {
struct QuickLink {
    uint64_t id = 0;
    std::wstring name;
    LocationKey target;
};
class QuickAccess {
  public:
    std::vector<QuickLink> links;
    uint64_t nextId = 1;
    bool readOnly = false;
    uint64_t Put(LocationStore &locations, const LocationKey &target, const std::wstring &name);
    bool Move(size_t index, int direction);
    bool Remove(size_t index);
    bool Save(LocationStore &locations, const std::filesystem::path &file) const;
    void Load(LocationStore &locations, const std::filesystem::path &file);
};
} // namespace zen
