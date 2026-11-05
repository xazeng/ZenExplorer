#pragma once
#include "operations.h"
#include <list>
namespace zen {
struct TaskTicket {
    std::filesystem::path result;
    // 仅观察进程是否退出；析构只关闭句柄，不终止任务。
    std::shared_ptr<void> process;
};
struct TaskCompletion {
    enum class Status { Result, Unreadable, Missing } status = Status::Missing;
    std::optional<TaskResult> result;
    std::optional<TaskItems> items;
};
enum class TaskFeedbackKind { None, Paste, Drop };
class TaskTracker {
  public:
    TaskTracker() = default;
    TaskTracker(const TaskTracker &) = delete;
    TaskTracker &operator=(const TaskTracker &) = delete;
    // 返回 false 表示界面反馈尚未完成，例如等待 Shell 枚举出新文件。
    using Feedback = std::function<bool(const TaskCompletion &)>;
    void Track(TaskTicket ticket, TaskFeedbackKind kind = TaskFeedbackKind::None, Feedback feedback = {});
    void Poll();
    size_t Pending(TaskFeedbackKind kind) const;
    size_t Count() const {
        return entries.size();
    }

  private:
    struct Entry {
        TaskTicket ticket;
        TaskFeedbackKind kind;
        Feedback feedback;
        std::optional<TaskCompletion> completion;
        bool consumed = false;
    };
    std::list<Entry> entries;
    bool polling = false;
};
} // namespace zen
