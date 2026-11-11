#include "operations.h"
#include "native_transfer.h"
#include "task_outcomes.h"
#include <wrl/implements.h>
#include <sherrors.h>
#include <unordered_set>
#include <shldisp.h>
using namespace zen;
// 只记录格式声明，不主动读取内容，避免诊断消耗远程延迟提供的数据。
static void LogPasteSource(DWORD expected, IDataObject *data) {
    if (!FileLogEnabled())
        return;
    DWORD ownerProcess = 0;
    GetWindowThreadProcessId(GetClipboardOwner(), &ownerProcess);
    Log(L"粘贴来源诊断 expected=" + std::to_wstring(expected) + L" current=" +
        std::to_wstring(GetClipboardSequenceNumber()) + L" ownerPid=" + std::to_wstring(ownerProcess));
    if (!data)
        return;
    ComPtr<IEnumFORMATETC> formats;
    HRESULT hr = data->EnumFormatEtc(DATADIR_GET, &formats);
    if (FAILED(hr) || !formats) {
        Log(L"粘贴格式枚举失败 hr=" + std::to_wstring((unsigned long)hr));
        return;
    }
    FORMATETC format{};
    for (unsigned count = 0; count < 64 && formats->Next(1, &format, nullptr) == S_OK; ++count) {
        wchar_t name[256]{};
        GetClipboardFormatNameW(format.cfFormat, name, 256);
        Log(L"粘贴格式 id=" + std::to_wstring(format.cfFormat) + L" name=" + name + L" tymed=" +
            std::to_wstring(format.tymed) + L" index=" + std::to_wstring(format.lindex));
        CoTaskMemFree(format.ptd);
        format = {};
    }
}
// 逐项记录系统结果，避免部分失败被整体返回值掩盖。
class TaskProgress final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IFileOperationProgressSink> {
  public:
    HRESULT failure = S_OK;
    bool incomplete = false;
    bool collectAll = false;
    TaskOutcomes outcomes;
    std::unordered_set<std::wstring> roots, seen;
    std::vector<std::wstring> created;
    HRESULT Record(IShellItem *source, IShellItem *newItem, HRESULT hr) {
        auto name = ItemName(source);
        outcomes.Record(name, hr);
        std::optional<std::wstring> createdName;
        // 只回传提交的顶层项目，避免把复制文件夹的后代也作为待选项目。
        if (SUCCEEDED(hr) && hr != COPYENGINE_S_USER_IGNORED && hr != COPYENGINE_S_PENDING && newItem &&
            (collectAll || roots.contains(name))) {
            createdName = ItemName(newItem);
            if (!createdName->empty() && seen.insert(*createdName).second)
                created.push_back(*createdName);
        }
        if (FileLogEnabled()) {
            if (!createdName)
                createdName = ItemName(newItem);
            Log(L"文件任务逐项结果 source=" + name + L" created=" + *createdName + L" hr=" +
                std::to_wstring((unsigned long)hr));
        }
        return S_OK;
    }
    void Summarize() {
        auto summary = outcomes.Summarize(failure);
        failure = summary.failure;
        incomplete |= summary.incomplete;
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT hr) override {
        if (FAILED(hr) && SUCCEEDED(failure))
            failure = hr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *s, LPCWSTR, HRESULT hr,
                                             IShellItem *n) override {
        return Record(s, n, hr);
    }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *s, IShellItem *, LPCWSTR, HRESULT hr,
                                           IShellItem *n) override {
        return Record(s, n, hr);
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *s, IShellItem *, LPCWSTR, HRESULT hr,
                                           IShellItem *n) override {
        return Record(s, n, hr);
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *s, HRESULT hr, IShellItem *n) override {
        return Record(s, n, hr);
    }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *s, LPCWSTR, LPCWSTR, DWORD, HRESULT hr,
                                          IShellItem *n) override {
        return Record(s, n, hr);
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResetTimer() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PauseTimer() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override {
        return S_OK;
    }
};

int zen::RunFileTask(HINSTANCE instance, const std::filesystem::path &file) {
    if (file.empty())
        return 2;
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"ZenExplorer 文件操作", WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, instance, nullptr);
    TaskResult result;
    TaskItems details;
    ComPtr<IDataObject> sourceFeedback, clipboard;
    DWORD pasteSequence = 0;
    bool systemOwnsErrors = false;
    try {
        if (!owner)
            throw HRESULT_FROM_WIN32(GetLastError());
        auto task = ReadTask(file);
        if (!task.sourceFeedback.empty()) {
            if (FileLogEnabled())
                Log(L"拖放来源回执开始解封送");
            ComPtr<IStream> stream;
            stream.Attach(SHCreateMemStream(task.sourceFeedback.data(), (UINT)task.sourceFeedback.size()));
            HRESULT hr =
                stream ? CoUnmarshalInterface(stream.Get(), IID_PPV_ARGS(&sourceFeedback)) : E_OUTOFMEMORY;
            if (FileLogEnabled()) {
                if (FAILED(hr))
                    Log(L"来源回执接口不可用，独立文件操作继续 " + ErrorText(hr));
                else
                    Log(L"拖放来源回执解封送成功");
            }
        }
        Operation operation = task.operation;
        bool nativePaste = false;
        if (operation == Operation::Paste) {
            pasteSequence = task.clipboardSequence;
            LogPasteSource(pasteSequence, nullptr);
            if (GetClipboardSequenceNumber() != pasteSequence)
                throw HRESULT_FROM_WIN32(ERROR_RETRY);
            HRESULT hr = OleGetClipboard(&clipboard);
            if (FAILED(hr))
                throw hr;
            LogPasteSource(pasteSequence, clipboard.Get());
            auto target = ParseItem(task.destination);
            SFGAOF attributes = 0;
            if (!target || FAILED(target->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes)) ||
                (attributes & (SFGAO_FILESYSTEM | SFGAO_FOLDER)) != (SFGAO_FILESYSTEM | SFGAO_FOLDER))
                throw HRESULT_FROM_WIN32(ERROR_DIRECTORY);
            task.destination = ItemName(target.Get(), SIGDN_FILESYSPATH);
            task.preferred = PreferredEffect(clipboard.Get());
            if (!SnapshotFiles(clipboard.Get(), task.sources)) {
                if (task.preferred & DROPEFFECT_MOVE)
                    throw E_NOTIMPL;
                nativePaste = true;
            }
            if (GetClipboardSequenceNumber() != pasteSequence)
                throw HRESULT_FROM_WIN32(ERROR_RETRY);
            Log(L"粘贴来源已绑定 kind=" + std::wstring(nativePaste ? L"系统数据对象" : L"文件路径"));
            operation = (task.preferred & DROPEFFECT_MOVE) ? Operation::Move : Operation::Copy;
        }
        if (operation == Operation::Drop) {
            if ((task.keys & MK_RBUTTON) ||
                (task.preferred != DROPEFFECT_COPY && task.preferred != DROPEFFECT_MOVE) ||
                !(task.allowed & task.preferred))
                throw E_INVALIDARG;
            operation = task.preferred == DROPEFFECT_MOVE ? Operation::Move : Operation::Copy;
        }
        ComPtr<IShellItem> destination;
        if (operation != Operation::Delete) {
            destination = ParseItem(task.destination);
            if (!destination)
                throw HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        }
        if (nativePaste) {
            // 系统负责描述解析、内容读取、冲突及进度；不创建项目私有接收目录。
            result.invoked = NativePaste(clipboard.Get(), destination.Get(), &systemOwnsErrors);
            if (FAILED(result.invoked))
                throw result.invoked;
            // Drop 的返回效果不是完成清单；不据此推断创建项目或清除剪切状态。
        } else {
            // 只对同目录复制启用系统副本命名；其他冲突交给系统询问。
            std::vector<ComPtr<IShellItem>> normal, duplicates;
            for (const auto &path : task.sources) {
                auto item = ParseItem(path);
                if (!item)
                    throw HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
                ComPtr<IShellItem> parent;
                int order = 1;
                bool duplicate = operation == Operation::Copy && SUCCEEDED(item->GetParent(&parent)) &&
                                 SUCCEEDED(parent->Compare(destination.Get(), SICHINT_CANONICAL, &order)) &&
                                 order == 0;
                (duplicate ? duplicates : normal).push_back(item);
            }
            HRESULT hr = S_OK;
            bool incomplete = false;
            details.destination = task.destination;
            for (int batch = 0; batch < 2 && SUCCEEDED(hr) && !incomplete; ++batch) {
                auto &items = batch ? duplicates : normal;
                if (items.empty())
                    continue;
                ComPtr<IFileOperation> op;
                hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&op));
                if (FAILED(hr))
                    throw hr;
                hr = op->SetOwnerWindow(owner);
                if (FAILED(hr))
                    throw hr;
                DWORD flags = FOF_NOCONFIRMMKDIR;
                if (!task.permanent)
                    flags |= FOF_ALLOWUNDO | FOFX_ADDUNDORECORD;
                if (operation == Operation::Delete && !task.permanent)
                    flags |= FOFX_RECYCLEONDELETE;
                if (batch)
                    flags |= FOF_RENAMEONCOLLISION | FOFX_PRESERVEFILEEXTENSIONS;
                hr = op->SetOperationFlags(flags);
                if (FAILED(hr))
                    throw hr;
                auto sink = Microsoft::WRL::Make<TaskProgress>();
                if (!sink)
                    throw E_OUTOFMEMORY;
                for (auto &item : items) {
                    sink->roots.insert(ItemName(item.Get()));
                    switch (operation) {
                    case Operation::Copy:
                        hr = op->CopyItem(item.Get(), destination.Get(), nullptr, nullptr);
                        break;
                    case Operation::Move:
                        hr = op->MoveItem(item.Get(), destination.Get(), nullptr, nullptr);
                        break;
                    case Operation::Delete:
                        hr = op->DeleteItem(item.Get(), nullptr);
                        break;
                    default:
                        hr = E_INVALIDARG;
                    }
                    if (FAILED(hr))
                        throw hr;
                }
                DWORD cookie = 0;
                hr = op->Advise(sink.Get(), &cookie);
                if (FAILED(hr))
                    throw hr;
                if (FileLogEnabled())
                    Log(L"独立 IFileOperation 开始 operation=" + std::to_wstring((UINT)operation) +
                        L" count=" + std::to_wstring(items.size()) + L" destination=" + task.destination);
                hr = op->PerformOperations();
                BOOL aborted = FALSE;
                HRESULT status = op->GetAnyOperationsAborted(&aborted);
                op->Unadvise(cookie);
                sink->Summarize();
                if (SUCCEEDED(hr) && FAILED(sink->failure))
                    hr = sink->failure;
                if (SUCCEEDED(hr) && FAILED(status))
                    hr = status;
                incomplete = aborted || sink->incomplete || FAILED(status);
                details.created.insert(details.created.end(), sink->created.begin(), sink->created.end());
            }
            result.invoked = SUCCEEDED(hr) && incomplete ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : hr;
            if (SUCCEEDED(result.invoked)) {
                result.effect = operation == Operation::Copy   ? DROPEFFECT_COPY
                                : operation == Operation::Move ? DROPEFFECT_MOVE
                                                               : DROPEFFECT_NONE;
                // 只有全部移动完成才清理仍对应本次任务的剪切状态。
                result.moveCompleted =
                    operation == Operation::Move && details.created.size() == task.sources.size();
            }
            if (FAILED(result.invoked) || !task.collectResults ||
                details.created.size() != task.sources.size())
                details.created.clear();
        }
        if (pasteSequence && result.moveCompleted && GetClipboardSequenceNumber() == pasteSequence)
            OleSetClipboard(nullptr);
    } catch (HRESULT hr) {
        result.invoked = hr;
        if (!systemOwnsErrors && !TaskCancelled(hr))
            Report(owner, L"文件操作未完成。来源可能已改变或连接已中断；已完成的文件予以保留。", hr);
    } catch (...) {
        result.invoked = E_INVALIDARG;
        Log(L"文件操作任务无效或已损坏");
    }
    if (sourceFeedback) {
        // 移动由目标完整执行，不能再让来源删除一次；仅在真正完成后反馈。
        SetDataEffect(sourceFeedback.Get(), CFSTR_PERFORMEDDROPEFFECT,
                      result.effect == DROPEFFECT_MOVE ? DROPEFFECT_NONE : result.effect);
        SetDataEffect(sourceFeedback.Get(), CFSTR_LOGICALPERFORMEDDROPEFFECT, result.effect);
        ComPtr<IDataObjectAsyncCapability> async;
        if (SUCCEEDED(sourceFeedback.As(&async))) {
            if (FileLogEnabled())
                Log(L"拖放来源完成回执开始");
            HRESULT hr = async->EndOperation(result.invoked, nullptr, result.effect);
            if (FileLogEnabled())
                Log(L"拖放来源完成回执 hr=" + std::to_wstring((unsigned long)hr));
        }
    }
    auto done = file;
    done += L".result";
    if (SUCCEEDED(result.invoked) && !details.created.empty()) {
        Writer detail;
        detail.Text(details.destination);
        detail.Pod((uint32_t)details.created.size());
        for (auto &path : details.created)
            detail.Text(path);
        auto itemsFile = done;
        itemsFile += L".items";
        if (!WriteEnvelope(itemsFile, TaskItemsMagic, detail.bytes, false) && FileLogEnabled())
            Log(L"结果明细写入失败，跳过自动选中");
    }
    Writer writer;
    writer.Pod(result.invoked);
    writer.Pod(result.moveCompleted);
    writer.Pod(result.effect);
    if (!WriteEnvelope(done, TaskMagic, writer.bytes, false) && FileLogEnabled())
        Log(L"文件操作结果写入失败");
    DeleteFileW(file.c_str());
    if (FileLogEnabled())
        Log(L"独立 Shell 文件操作结束 hr=" + std::to_wstring((unsigned long)result.invoked) + L" effect=" +
            std::to_wstring(result.effect));
    if (owner)
        DestroyWindow(owner);
    return FAILED(result.invoked) ? 1 : 0;
}
