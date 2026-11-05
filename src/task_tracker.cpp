#include "task_tracker.h"
namespace zen {
static bool Exited(const TaskTicket &ticket) {
    return ticket.process && WaitForSingleObject(ticket.process.get(), 0) == WAIT_OBJECT_0;
}
static std::optional<TaskCompletion> ReadCompletion(const TaskTicket &ticket) {
    std::error_code ec;
    bool ready = std::filesystem::exists(ticket.result, ec);
    if (!ready) {
        if (!Exited(ticket))
            return std::nullopt;
        // 进程退出可能早于首次 exists，重新检查已落盘的结果。
        ready = std::filesystem::exists(ticket.result, ec);
        if (!ready) {
            Log(L"操作进程已退出但没有结果 " + ticket.result.wstring());
            return TaskCompletion{};
        }
    }
    TaskCompletion completion;
    try {
        completion.result = ReadTaskResult(ticket.result);
        completion.status = TaskCompletion::Status::Result;
    } catch (...) {
        completion.status = TaskCompletion::Status::Unreadable;
        Log(L"任务结果不可用，不猜测完成状态 " + ticket.result.wstring());
        return completion;
    }
    auto details = ticket.result;
    details += L".items";
    if (std::filesystem::exists(details, ec)) {
        try {
            completion.items = ReadTaskItems(ticket.result);
        } catch (...) {
            Log(L"任务结果明细不可用，跳过自动选中");
        }
    }
    return completion;
}
void TaskTracker::Track(TaskTicket ticket, TaskFeedbackKind kind, Feedback feedback) {
    if (ticket.result.empty())
        return;
    bool consumed = !feedback;
    entries.push_back({std::move(ticket), kind, std::move(feedback), {}, consumed});
}
size_t TaskTracker::Pending(TaskFeedbackKind kind) const {
    return std::count_if(entries.begin(), entries.end(),
                         [&](const Entry &entry) { return entry.kind == kind && !entry.consumed; });
}
void TaskTracker::Poll() {
    if (polling)
        return;
    polling = true;
    struct Reset {
        bool &flag;
        ~Reset() {
            flag = false;
        }
    } reset{polling};
    // Shell 反馈可能重入消息循环或提交新任务；新任务留到下一轮处理。
    size_t remaining = entries.size();
    for (auto it = entries.begin(); it != entries.end() && remaining > 0; --remaining) {
        auto &entry = *it;
        if (!entry.consumed) {
            if (!entry.completion)
                entry.completion = ReadCompletion(entry.ticket);
            if (entry.completion) {
                try {
                    entry.consumed = entry.feedback(*entry.completion);
                } catch (...) {
                    Log(L"任务界面反馈失败，停止重试反馈");
                    entry.consumed = true;
                }
                if (entry.consumed) {
                    entry.feedback = {};
                    entry.completion.reset();
                }
            }
            // 与原行为一致，消费结果和确认写入进程退出分别在两轮处理。
            ++it;
            continue;
        }
        if (!entry.ticket.process) {
            it = entries.erase(it); // 无退出凭据时不删除磁盘结果。
        } else if (Exited(entry.ticket)) {
            if (!CleanupTaskResult(entry.ticket.result))
                Log(L"任务结果清理未完成 " + entry.ticket.result.filename().wstring());
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
}
} // namespace zen
