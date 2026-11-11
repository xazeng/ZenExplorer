#pragma once
#include "storage.h"
namespace zen {
enum class Operation : uint32_t { Copy = 1, Move = 2, Delete = 3, Paste = 4, Drop = 5 };
struct FileTask {
    Operation operation = Operation::Copy;
    std::wstring destination;
    std::vector<std::wstring> sources;
    bool permanent = false;
    DWORD keys = 0;
    DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    DWORD preferred = DROPEFFECT_NONE;
    POINTL point{};
    bool collectResults = true;
    // 仅用于通知普通文件拖放来源完成；文件操作不通过此对象读取内容。
    std::vector<uint8_t> sourceFeedback;
    // 粘贴只提交意图和剪贴板版本，来源由操作进程在交付阶段取得。
    DWORD clipboardSequence = 0;
};
// 开发阶段采用新的任务格式，不读取旧操作任务；会话格式不变。
inline constexpr uint32_t TaskMagic = 0x334E455A;
inline constexpr uint32_t MaxTaskItems = 100000;
inline constexpr uint32_t MaxTaskFeedbackBytes = 1024 * 1024;
// 校验实际待写入的回执包大小，成功时返回不含封装头的序列化长度；不访问 Shell 或文件系统。
HRESULT ValidateTaskPayload(const FileTask &task, size_t feedbackBytes, size_t *payloadBytes = nullptr);
struct TaskResult {
    HRESULT invoked = E_FAIL;
    bool moveCompleted = false;
    DWORD effect = DROPEFFECT_NONE;
};
TaskResult ReadTaskResult(const std::filesystem::path &result);
bool TaskCancelled(HRESULT result);
// 可选系统结果选择独立于命令状态；没有明细时保留主视图原选择。
inline constexpr uint32_t TaskItemsMagic = 0x4954455A;
struct TaskItems {
    std::wstring destination;
    std::vector<std::wstring> created;
};
TaskItems ReadTaskItems(const std::filesystem::path &result);
// 仅删除任务目录中已消费结果的精确文件；调用者须确认写入进程已退出。
bool CleanupTaskResult(const std::filesystem::path &result);
// 仅供本次跟踪结果消费后调用；再次检查退出凭据及 GUID 文件名，精确回收任务与临时文件。
// 不扫描目录，不解释或释放封送数据，也不终止进程。
bool CleanupExitedTaskFiles(const std::filesystem::path &result, HANDLE process);
// 测试可指定待验证程序；生产调用默认启动当前可执行文件。
bool LaunchTask(HWND owner, const FileTask &task, const std::filesystem::path &executable = {},
                std::filesystem::path *resultFile = nullptr, std::shared_ptr<void> *process = nullptr,
                IDataObject *dropSource = nullptr, DWORD *dropReceipt = nullptr);
// 由已初始化 OLE 的任务模式入口调用，不创建主窗口或参与单实例。
int RunFileTask(HINSTANCE instance, const std::filesystem::path &file);
FileTask ReadTask(const std::filesystem::path &file);
std::vector<std::wstring> DataItems(IDataObject *object);
DWORD PreferredEffect(IDataObject *object);
std::optional<DWORD> DataEffect(IDataObject *object, const wchar_t *format);
HRESULT SetDataEffect(IDataObject *object, const wchar_t *format, DWORD effect);
// 仅快照可重新解析的文件系统项目，不能把虚拟文件内容冒充文件路径。
bool SnapshotFiles(IDataObject *object, std::vector<std::wstring> &paths);
HRESULT TaskShellItems(const std::vector<std::wstring> &paths, IShellItemArray **items);
HRESULT SystemDropTarget(IShellItem *destination, IDropTarget **target);
} // namespace zen
