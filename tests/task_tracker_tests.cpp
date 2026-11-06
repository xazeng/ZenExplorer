#include "task_tracker.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace zen;
#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition))                                                                                    \
            throw std::runtime_error(#condition);                                                            \
    } while (false)
static std::shared_ptr<void> ExitSignal(bool signalled = false) {
    auto signal = std::shared_ptr<void>(CreateEventW(nullptr, TRUE, signalled, nullptr),
                                        [](void *handle) { CloseHandle(handle); });
    CHECK(signal);
    return signal;
}
static std::filesystem::path ResultFile(const wchar_t *name) {
    return DataDir() / L"tasks" / (std::wstring(name) + L".task.result");
}
static void WriteResult(const std::filesystem::path &path) {
    Writer writer;
    writer.Pod<HRESULT>(S_OK);
    writer.Pod<bool>(false);
    writer.Pod<DWORD>(DROPEFFECT_COPY);
    CHECK(WriteEnvelope(path, TaskMagic, writer.bytes, false));
}
static void CompletionBeforeExit() {
    TaskTracker tracker;
    auto process = ExitSignal();
    auto file = ResultFile(L"completion-before-exit");
    int callbacks = 0;
    tracker.Track({file, process}, TaskFeedbackKind::Paste, [&](const TaskCompletion &completion) {
        CHECK(completion.result && completion.result->invoked == S_OK);
        ++callbacks;
        return callbacks == 2;
    });
    tracker.Poll();
    CHECK(callbacks == 0);
    WriteResult(file);
    tracker.Poll();
    CHECK(callbacks == 1 && tracker.Pending(TaskFeedbackKind::Paste) == 1);
    tracker.Poll();
    CHECK(callbacks == 2 && tracker.Pending(TaskFeedbackKind::Paste) == 0);
    tracker.Poll();
    CHECK(tracker.Count() == 1 && std::filesystem::exists(file));
    CHECK(SetEvent(process.get()));
    tracker.Poll();
    CHECK(tracker.Count() == 0 && !std::filesystem::exists(file) && callbacks == 2);
}
static void MissingAndCorruptResults() {
    for (bool corrupt : {false, true}) {
        TaskTracker tracker;
        auto file = ResultFile(corrupt ? L"corrupt" : L"missing");
        if (corrupt) {
            std::filesystem::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary) << "invalid result";
        }
        int callbacks = 0;
        tracker.Track({file, ExitSignal(true)}, TaskFeedbackKind::Drop,
                      [&](const TaskCompletion &completion) {
                          CHECK(!completion.result);
                          CHECK(completion.status == (corrupt ? TaskCompletion::Status::Unreadable
                                                              : TaskCompletion::Status::Missing));
                          ++callbacks;
                          return true;
                      });
        tracker.Poll();
        tracker.Poll();
        CHECK(callbacks == 1 && tracker.Count() == 0);
    }
}
static void ReentrantFeedback() {
    TaskTracker tracker;
    auto first = ResultFile(L"reentrant-first"), second = ResultFile(L"reentrant-second");
    WriteResult(first);
    WriteResult(second);
    int firstCalls = 0, secondCalls = 0;
    tracker.Track({first, {}}, TaskFeedbackKind::Paste, [&](const TaskCompletion &) {
        ++firstCalls;
        tracker.Track({second, {}}, TaskFeedbackKind::Drop, [&](const TaskCompletion &) {
            ++secondCalls;
            return true;
        });
        tracker.Poll();
        return true;
    });
    tracker.Poll();
    CHECK(firstCalls == 1 && secondCalls == 0);
    tracker.Poll();
    tracker.Poll();
    CHECK(firstCalls == 1 && secondCalls == 1 && tracker.Count() == 0);
    // 没有进程退出凭据时，只释放跟踪记录，不擅自删除结果。
    CHECK(std::filesystem::exists(first) && std::filesystem::exists(second));
}
static void InvalidOptionalDetails() {
    TaskTracker tracker;
    auto file = ResultFile(L"invalid-details");
    WriteResult(file);
    auto details = file;
    details += L".items";
    std::ofstream(details, std::ios::binary) << "invalid details";
    int callbacks = 0;
    tracker.Track({file, ExitSignal(true)}, TaskFeedbackKind::Paste, [&](const TaskCompletion &completion) {
        CHECK(completion.result && completion.result->invoked == S_OK && !completion.items);
        ++callbacks;
        return true;
    });
    tracker.Poll();
    tracker.Poll();
    CHECK(callbacks == 1 && !std::filesystem::exists(file) && !std::filesystem::exists(details));
}
static void DestructionDoesNotCancel() {
    auto process = ExitSignal();
    auto file = ResultFile(L"still-running");
    WriteResult(file);
    {
        TaskTracker tracker;
        tracker.Track({file, process});
        tracker.Poll();
        CHECK(tracker.Count() == 1);
    }
    CHECK(WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT);
    CHECK(std::filesystem::exists(file));
}
static std::shared_ptr<void> PreventDeletion(const std::filesystem::path &file) {
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    return std::shared_ptr<void>(handle, [](void *value) { CloseHandle(value); });
}
static void CleanupRetriesWithoutRepeatingFeedback() {
    TaskTracker tracker;
    auto file = ResultFile(L"cleanup-retry");
    WriteResult(file);
    auto locked = PreventDeletion(file);
    int callbacks = 0;
    tracker.Track({file, ExitSignal(true)}, TaskFeedbackKind::Paste, [&](const TaskCompletion &completion) {
        CHECK(completion.result && completion.result->invoked == S_OK);
        ++callbacks;
        return true;
    });
    tracker.Poll(1000); // 消费结果。
    tracker.Poll(1000); // 首次清理因占用失败。
    CHECK(callbacks == 1 && tracker.Count() == 1 && tracker.Pending(TaskFeedbackKind::Paste) == 0);
    locked.reset();
    tracker.Poll(1999);
    CHECK(tracker.Count() == 1 && std::filesystem::exists(file));
    tracker.Poll(2000);
    CHECK(callbacks == 1 && tracker.Count() == 0 && !std::filesystem::exists(file));
}
static void PermanentCleanupFailureReleasesObservation() {
    TaskTracker tracker;
    auto file = ResultFile(L"cleanup-permanent-failure");
    WriteResult(file);
    auto locked = PreventDeletion(file);
    auto process = ExitSignal(true);
    std::weak_ptr<void> observation = process;
    tracker.Track({file, process});
    process.reset();
    tracker.Poll(0);
    CHECK(tracker.Count() == 1 && !observation.expired());
    // 单调时钟替代真实等待，永久占用不能使观察句柄无限保留。
    for (ULONGLONG minute = 1; minute <= 8 && tracker.Count(); ++minute)
        tracker.Poll(minute * 60000);
    CHECK(tracker.Count() == 0 && observation.expired() && std::filesystem::exists(file));
    locked.reset();
    CHECK(CleanupTaskResult(file));
}
int main() {
    auto root = std::filesystem::current_path() / L".test-data" /
                (L"task-tracker-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()));
    SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", root.c_str());
    SetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", nullptr);
    struct Case {
        const char *name;
        void (*run)();
    };
    bool failed = false;
    for (const auto &test : {Case{"completion_before_exit", CompletionBeforeExit},
                             Case{"missing_and_corrupt_results", MissingAndCorruptResults},
                             Case{"reentrant_feedback", ReentrantFeedback},
                             Case{"invalid_optional_details", InvalidOptionalDetails},
                             Case{"destruction_does_not_cancel", DestructionDoesNotCancel},
                             Case{"cleanup_retry", CleanupRetriesWithoutRepeatingFeedback},
                             Case{"cleanup_permanent_failure", PermanentCleanupFailureReleasesObservation}}) {
        try {
            test.run();
            std::cout << test.name << ": passed\n";
        } catch (const std::exception &error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
