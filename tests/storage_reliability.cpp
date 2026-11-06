#include "storage.h"
#include <fstream>
#include <stdexcept>
using namespace zen;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)

namespace {
struct ReadLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit ReadLock(const std::filesystem::path &file)
        : handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr)) {
        CHECK(handle != INVALID_HANDLE_VALUE);
    }
    ~ReadLock() {
        CloseHandle(handle);
    }
};
size_t FileCount(const std::filesystem::path &folder) {
    if (!std::filesystem::exists(folder))
        return 0;
    return (size_t)std::distance(std::filesystem::directory_iterator(folder),
                                 std::filesystem::directory_iterator{});
}
void CheckRepeatedCommitFailure() {
    Model original;
    auto id = original.panes[0].selected;
    original.CommitNavigation(id, L"C:\\reliable-home", {L"C:\\reliable-home"});
    original.Rename(id, L"需要保留的主目录标签");
    original.CommitNavigation(id, L"C:\\reliable-current", {L"C:\\reliable-current"});
    original.wrap = false;
    original.Find(id)->viewMode = FVM_LIST;
    const auto file = ConfigDir() / L"reliable-commit.dat";
    const auto tabs = TabsPath(file);
    CHECK(SaveModel(original, file));
    const auto savedTabs = ReadEnvelope(tabs, 0x544E455A, 2);
    const auto savedSession = ReadEnvelope(file, 0x434E455A, 7);
    const auto history = DataDir() / L"history" / file.stem();
    const auto locations = DataDir() / L"locations" / file.stem();
    Model changed = original;
    changed.wrap = true;
    changed.CommitNavigation(id, L"C:\\uncommitted", {L"C:\\uncommitted"});
    {
        // 保持标签可读，但拒绝替换；附属会话写入成功后，连续三次模拟提交失败。
        ReadLock locked(tabs);
        for (int attempt = 0; attempt < 3; ++attempt) {
            CHECK(!SaveModel(changed, file));
            CHECK(ReadEnvelope(tabs, 0x544E455A, 2) == savedTabs);
            CHECK(ReadEnvelope(file.wstring() + L".bak", 0x434E455A, 7) == savedSession);
            CHECK(FileCount(history) == 1 && FileCount(locations) == 1);
            CHECK(!std::filesystem::exists(tabs.wstring() + L".tmp"));
            Model recovered;
            bool backup = false, future = false;
            CHECK(LoadModel(recovered, file, backup, future));
            CHECK(!future && !recovered.wrap);
            CHECK(recovered.Find(id)->path == original.Find(id)->path);
            CHECK(recovered.Find(id)->history == original.Find(id)->history);
            CHECK(recovered.Find(id)->viewMode == FVM_LIST);
        }
    }
    CHECK(SaveModel(changed, file));
    CHECK(FileCount(history) == 2 && FileCount(locations) == 2);
    // 故障恢复后的第一次成功提交，仍须留下完整的上一代备份。
    std::ofstream(tabs, std::ios::binary | std::ios::trunc) << "broken";
    Model recovered;
    bool backup = false, future = false;
    CHECK(LoadModel(recovered, file, backup, future) && backup && !future);
    CHECK(!recovered.wrap && recovered.Find(id)->path == original.Find(id)->path);
    CHECK(recovered.Find(id)->history == original.Find(id)->history);
    CHECK(recovered.Find(id)->viewMode == FVM_LIST);
}
void CheckSnapshotRollback() {
    const auto file = ConfigDir() / L"reliable-rollback.dat";
    const auto history = DataDir() / L"history" / file.stem();
    const auto locations = DataDir() / L"locations" / file.stem();
    std::filesystem::create_directories(history.parent_path());
    std::ofstream(history, std::ios::binary) << "阻止历史目录创建";
    Model model;
    for (int attempt = 0; attempt < 3; ++attempt) {
        CHECK(!SaveModel(model, file));
        CHECK(FileCount(locations) == 0);
        CHECK(!std::filesystem::exists(TabsPath(file)));
    }
    CHECK(std::filesystem::remove(history));
    CHECK(SaveModel(model, file));
}
void CheckEnvelopeFailureCleanup() {
    const auto file = ConfigDir() / L"reliable-envelope.dat";
    const std::vector<uint8_t> original{1, 2, 3};
    CHECK(WriteEnvelope(file, 0x12345678, original, false));
    for (bool backup : {false, true}) {
        ReadLock locked(file);
        CHECK(!WriteEnvelope(file, 0x12345678, {4, 5, 6}, backup));
        CHECK(ReadEnvelope(file, 0x12345678) == original);
        CHECK(!std::filesystem::exists(file.wstring() + L".tmp"));
    }
    // 写端超限须在修改现有文件前拒绝，与读端的容量契约一致。
    std::vector<uint8_t> oversized(MaxEnvelopeBytes + 1, 0);
    CHECK(!WriteEnvelope(file, 0x12345678, oversized, true));
    CHECK(ReadEnvelope(file, 0x12345678) == original);
    CHECK(!std::filesystem::exists(file.wstring() + L".tmp"));
}
} // namespace

void CheckStorageReliability() {
    CheckRepeatedCommitFailure();
    CheckSnapshotRollback();
    CheckEnvelopeFailureCleanup();
}
