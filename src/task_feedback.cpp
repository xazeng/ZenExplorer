#include "task_feedback.h"
#include "app.h"
#include "shell_view.h"
namespace zen {
void FileTaskFeedback::TrackPaste(PasteFeedback pending) {
    lastPasteResult = pending.result;
    TaskTicket ticket{pending.result, std::move(pending.process)};
    tracker.Track(std::move(ticket), TaskFeedbackKind::Paste,
                  [this, pending = std::move(pending)](const TaskCompletion &completion) mutable {
                      return ApplyPaste(pending, completion);
                  });
}
void FileTaskFeedback::TrackDrop(DropFeedback pending) {
    TaskTicket ticket{pending.result, std::move(pending.process)};
    tracker.Track(std::move(ticket), TaskFeedbackKind::Drop,
                  [this, pending = std::move(pending)](const TaskCompletion &completion) {
                      if (completion.result) {
                          const auto &result = *completion.result;
                          bool cancelled = TaskCancelled(result.invoked) ||
                                           (SUCCEEDED(result.invoked) && result.effect == DROPEFFECT_NONE);
                          if (cancelled && pending.generation == app.dropGeneration)
                              app.RestoreGroups(pending.expanded);
                      }
                      return true;
                  });
}
void FileTaskFeedback::TrackDelete(TaskTicket ticket) {
    tracker.Track(std::move(ticket));
}
bool FileTaskFeedback::ApplyPaste(PasteFeedback &pending, const TaskCompletion &completion) {
    if (!completion.result || FAILED(completion.result->invoked))
        return true;
    if (!pending.read) {
        // 只有仍对应本次操作的剪切剪贴板才清空；后续复制不能被迟到结果影响。
        if (pending.move && completion.result->moveCompleted &&
            GetClipboardSequenceNumber() == pending.clipboardSequence)
            OleSetClipboard(nullptr);
        if (!pending.tab || !completion.items || completion.items->destination != pending.destination ||
            completion.items->created.empty())
            return true;
        pending.created = completion.items->created;
        pending.read = true;
        pending.deadline = GetTickCount64() + 5000;
    }
    if (!PasteCurrent(pending, GetForegroundWindow(), GetFocus()))
        return true;
    auto it = app.views.find(pending.tab);
    if (it == app.views.end())
        return true;
    HRESULT selection = it->second->SelectPasteResults(pending.created);
    if (selection == S_FALSE)
        return false;
    Log(L"粘贴结果选中 tab=" + std::to_wstring(pending.tab) + L" result=" +
        std::to_wstring((unsigned long)selection));
    return true;
}
bool FileTaskFeedback::PasteCurrent(const PasteFeedback &pending, HWND foreground, HWND focus) const {
    auto candidate = [&]() -> ShellView * {
        // 已失效的反馈不再查询 Shell，避免后台目录或扩展拖慢结果消费。
        if (focus != pending.focus || foreground != app.window || GetTickCount64() > pending.deadline ||
            pending.pane < 0 || pending.pane >= 2 || app.pasteGeneration != pending.generation ||
            app.model.active != pending.pane || app.model.PaneOf(pending.tab) != pending.pane ||
            app.model.panes[pending.pane].selected != pending.tab)
            return nullptr;
        auto it = app.views.find(pending.tab);
        return it != app.views.end() && !it->second->Navigation().Pending() ? it->second.get() : nullptr;
    };
    auto *source = candidate();
    if (!source)
        return false;
    const auto path = app.model.Find(pending.tab)->path;
    HWND liveForeground = GetForegroundWindow(), liveFocus = GetFocus();
    ComPtr<IShellView> shell = source->shell;
    auto *identity = shell.Get();
    if (candidate() != source || source->shell.Get() != identity)
        return false;
    bool sameDestination = ItemName(source->Current().Get()) == pending.destination;
    // Shell 查询及接口释放均可能重入；只接受仍属于原视图和交互状态的结果。
    shell.Reset();
    auto *current = candidate();
    return sameDestination && current == source && current->shell.Get() == identity &&
           app.model.Find(pending.tab)->path == path && GetForegroundWindow() == liveForeground &&
           GetFocus() == liveFocus;
}
} // namespace zen
