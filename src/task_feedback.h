#pragma once
#include "task_tracker.h"
namespace zen {
class App;
struct PasteFeedback {
    std::filesystem::path result;
    DWORD clipboardSequence = 0;
    bool move = true;
    Id tab = 0;
    int pane = -1;
    uint64_t generation = 0;
    HWND focus = nullptr;
    std::wstring destination;
    std::vector<std::wstring> created;
    bool read = false;
    ULONGLONG deadline = 0;
    std::shared_ptr<void> process;
};
struct DropFeedback {
    std::filesystem::path result;
    std::shared_ptr<void> process;
    std::vector<GroupExpansionState> expanded;
    uint64_t generation = 0;
};
class FileTaskFeedback {
    App &app;
    TaskTracker tracker;
    std::filesystem::path lastPasteResult;
    bool ApplyPaste(PasteFeedback &pending, const TaskCompletion &completion);

  public:
    explicit FileTaskFeedback(App &owner) : app(owner) {}
    void TrackPaste(PasteFeedback pending);
    void TrackDrop(DropFeedback pending);
    void TrackDelete(TaskTicket ticket);
    void Poll() {
        tracker.Poll();
    }
    bool PasteCurrent(const PasteFeedback &pending, HWND foreground, HWND focus) const;
    size_t PendingPastes() const {
        return tracker.Pending(TaskFeedbackKind::Paste);
    }
    size_t TrackedCount() const {
        return tracker.Count();
    }
    const std::filesystem::path &LastPasteResult() const {
        return lastPasteResult;
    }
};
} // namespace zen
