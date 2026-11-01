#include "operations.h"
#include <wrl/implements.h>
#include <sherrors.h>
#include <unordered_set>
#include <unordered_map>
#include <shldisp.h>
using namespace zen;
// 逐项记录系统结果，避免部分失败被整体返回值掩盖。
class TaskProgress final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IFileOperationProgressSink> {
  public:
    HRESULT failure = S_OK;
    bool incomplete = false;
    std::unordered_map<std::wstring, HRESULT> outcomes;
    std::unordered_set<std::wstring> roots, seen;
    std::vector<std::wstring> created;
    HRESULT Record(IShellItem *source, IShellItem *newItem, HRESULT hr) {
        auto name = ItemName(source);
        outcomes[name] = hr;
        // 只回传提交的顶层项目，避免把复制文件夹的后代也作为待选项目。
        if (SUCCEEDED(hr) && hr != COPYENGINE_S_USER_IGNORED && hr != COPYENGINE_S_PENDING && newItem &&
            roots.contains(name)) {
            auto path = ItemName(newItem);
            if (!path.empty() && seen.insert(path).second)
                created.push_back(path);
        }
        Log(L"文件任务逐项结果 source=" + ItemName(source) + L" created=" + ItemName(newItem) + L" hr=" +
            std::to_wstring((unsigned long)hr));
        return S_OK;
    }
    void Summarize() {
        // 冲突询问会先回报 PENDING，再回报真正结果，不能永久把 PENDING 当成取消。
        for (auto &[path, hr] : outcomes) {
            if (FAILED(hr) && SUCCEEDED(failure))
                failure = hr;
            if (FAILED(hr) || hr == COPYENGINE_S_USER_IGNORED || hr == COPYENGINE_S_PENDING ||
                hr == COPYENGINE_S_PENDING_DELETE || hr == COPYENGINE_S_PENDING_BATCH_COPY)
                incomplete = true;
        }
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
    ComPtr<IDataObject> sourceFeedback;
    try {
        if (!owner)
            throw HRESULT_FROM_WIN32(GetLastError());
        auto task = ReadTask(file);
        if (!task.sourceFeedback.empty()) {
            Log(L"拖放来源回执开始解封送");
            ComPtr<IStream> stream;
            stream.Attach(SHCreateMemStream(task.sourceFeedback.data(), (UINT)task.sourceFeedback.size()));
            HRESULT hr =
                stream ? CoUnmarshalInterface(stream.Get(), IID_PPV_ARGS(&sourceFeedback)) : E_OUTOFMEMORY;
            if (FAILED(hr))
                Log(L"来源回执接口不可用，独立文件操作继续 " + ErrorText(hr));
            else
                Log(L"拖放来源回执解封送成功");
        }
        Operation operation = task.operation;
        if (operation == Operation::Paste)
            operation = task.preferred == DROPEFFECT_MOVE ? Operation::Move : Operation::Copy;
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
            Log(L"独立 IFileOperation 开始 operation=" + std::to_wstring((UINT)operation) + L" count=" +
                std::to_wstring(items.size()) + L" destination=" + task.destination);
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
        if (FAILED(result.invoked) || !task.collectResults || details.created.size() != task.sources.size())
            details.created.clear();
    } catch (HRESULT hr) {
        result.invoked = hr;
        if (!TaskCancelled(hr))
            Report(owner, L"无法提交系统文件操作", hr);
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
            Log(L"拖放来源完成回执开始");
            HRESULT hr = async->EndOperation(result.invoked, nullptr, result.effect);
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
        if (!WriteEnvelope(itemsFile, TaskItemsMagic, detail.bytes, false))
            Log(L"结果明细写入失败，跳过自动选中");
    }
    Writer writer;
    writer.Pod(result.invoked);
    writer.Pod(result.moveCompleted);
    writer.Pod(result.effect);
    if (!WriteEnvelope(done, TaskMagic, writer.bytes, false))
        Log(L"文件操作结果写入失败");
    DeleteFileW(file.c_str());
    Log(L"独立 IFileOperation 结束 hr=" + std::to_wstring((unsigned long)result.invoked) + L" effect=" +
        std::to_wstring(result.effect));
    if (owner)
        DestroyWindow(owner);
    return FAILED(result.invoked) ? 1 : 0;
}
