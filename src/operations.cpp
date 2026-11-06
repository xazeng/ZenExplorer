#include "operations.h"
#include <sherrors.h>
#include <shldisp.h>
namespace zen {
bool TaskCancelled(HRESULT result) {
    return result == E_ABORT || result == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
           result == COPYENGINE_E_USER_CANCELLED || result == COPYENGINE_E_CANCELLED;
}
HRESULT ValidateTaskPayload(const FileTask &task, size_t feedbackBytes, size_t *payloadBytes) {
    if (payloadBytes)
        *payloadBytes = 0;
    if (task.sources.empty() || task.operation < Operation::Copy || task.operation > Operation::Drop ||
        (task.operation != Operation::Delete && task.destination.empty()) ||
        (task.allowed & ~(DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)) ||
        (task.preferred & ~(DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)) ||
        (feedbackBytes && task.operation != Operation::Drop))
        return E_INVALIDARG;
    if (task.sources.size() > MaxTaskItems || feedbackBytes > MaxTaskFeedbackBytes)
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    // 固定字段包括目标字符串长度、源项目数量和回执包长度，字符串内容单独计入。
    size_t bytes = sizeof(Operation) + 2 * sizeof(bool) + 3 * sizeof(DWORD) + sizeof(POINTL) +
                   3 * sizeof(uint32_t) + feedbackBytes;
    auto addText = [&](const std::wstring &text, bool source) -> HRESULT {
        if (text.size() > MaxStoredTextLength)
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        if ((source && text.empty()) || text.find(L'\0') != std::wstring::npos)
            return E_INVALIDARG;
        size_t length = text.size() * sizeof(wchar_t) + (source ? sizeof(uint32_t) : 0);
        if (length > MaxEnvelopeBytes - bytes)
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        bytes += length;
        return S_OK;
    };
    HRESULT hr = addText(task.destination, false);
    if (FAILED(hr))
        return hr;
    for (const auto &source : task.sources) {
        hr = addText(source, true);
        if (FAILED(hr))
            return hr;
    }
    if (payloadBytes)
        *payloadBytes = bytes;
    return S_OK;
}
TaskResult ReadTaskResult(const std::filesystem::path &result) {
    auto bytes = ReadEnvelope(result, TaskMagic);
    Reader reader{bytes};
    TaskResult value{reader.Pod<HRESULT>(), reader.Pod<bool>(), reader.Pod<DWORD>()};
    if (reader.pos != bytes.size() ||
        (value.effect & ~(DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)) ||
        (value.moveCompleted && (FAILED(value.invoked) || value.effect != DROPEFFECT_MOVE)))
        throw std::runtime_error("无效系统命令结果");
    return value;
}
TaskItems ReadTaskItems(const std::filesystem::path &result) {
    auto detail = result;
    detail += L".items";
    auto bytes = ReadEnvelope(detail, TaskItemsMagic);
    Reader reader{bytes};
    TaskItems items;
    items.destination = reader.Text();
    for (auto count = reader.Count(MaxTaskItems); count--;)
        items.created.push_back(reader.Text());
    if (reader.pos != bytes.size() || items.destination.empty() ||
        std::any_of(items.created.begin(), items.created.end(), [](auto &s) { return s.empty(); }))
        throw std::runtime_error("无效粘贴结果明细");
    return items;
}
bool CleanupTaskResult(const std::filesystem::path &result) {
    std::error_code ec;
    auto root = std::filesystem::weakly_canonical(DataDir() / L"tasks", ec);
    if (ec || result.empty() || result.filename().wstring().find(L".task.result") == std::wstring::npos)
        return false;
    auto parent = std::filesystem::weakly_canonical(result.parent_path(), ec);
    if (ec || parent != root || !result.filename().wstring().ends_with(L".task.result"))
        return false;
    auto items = result;
    items += L".items";
    std::filesystem::remove(items, ec);
    if (ec)
        return false;
    std::filesystem::remove(result, ec);
    if (ec)
        return false;
    return true;
}
bool CleanupExitedTaskFiles(const std::filesystem::path &result, HANDLE process) {
    if (!process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
        return false;
    const auto name = result.filename().wstring();
    const std::wstring suffix = L".task.result";
    if (name.size() != 38 + suffix.size() || !name.ends_with(suffix))
        return false;
    const auto token = name.substr(0, 38);
    GUID id{};
    wchar_t canonical[40]{};
    if (token.front() != L'{' || token.back() != L'}' || FAILED(CLSIDFromString(token.c_str(), &id)) ||
        !StringFromGUID2(id, canonical, 40) || _wcsicmp(token.c_str(), canonical))
        return false;
    std::error_code ec;
    auto root = std::filesystem::weakly_canonical(DataDir() / L"tasks", ec);
    if (ec)
        return false;
    auto parent = std::filesystem::weakly_canonical(result.parent_path(), ec);
    if (ec || parent != root)
        return false;
    bool complete = true;
    // 名称由已验证的 GUID 构造，不能用通配符扩大到其他任务或未知附属文件。
    for (const auto *ending : {L".task.tmp", L".task", L".task.result.items.tmp", L".task.result.items",
                               L".task.result.tmp", L".task.result"}) {
        const auto file = root / (token + ending);
        DWORD attributes = GetFileAttributesW(file.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                complete = false;
            continue;
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            complete = false;
            continue;
        }
        if (!DeleteFileW(file.c_str())) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                complete = false;
        }
    }
    return complete;
}
FileTask ReadTask(const std::filesystem::path &file) {
    auto bytes = ReadEnvelope(file, TaskMagic);
    Reader r{bytes};
    FileTask t;
    t.operation = r.Pod<Operation>();
    t.permanent = r.Pod<bool>();
    t.destination = r.Text();
    for (auto n = r.Count(MaxTaskItems); n--;)
        t.sources.push_back(r.Text());
    t.keys = r.Pod<DWORD>();
    t.allowed = r.Pod<DWORD>();
    t.preferred = r.Pod<DWORD>();
    t.point = r.Pod<POINTL>();
    t.collectResults = r.Pod<bool>();
    for (auto n = r.Count(MaxTaskFeedbackBytes); n--;)
        t.sourceFeedback.push_back(r.Pod<uint8_t>());
    if (r.pos != bytes.size() || FAILED(ValidateTaskPayload(t, t.sourceFeedback.size())))
        throw std::runtime_error("无效文件任务");
    return t;
}
bool LaunchTask(HWND owner, const FileTask &t, const std::filesystem::path &executable,
                std::filesystem::path *resultFile, std::shared_ptr<void> *process, IDataObject *dropSource,
                DWORD *dropReceipt) {
    if (dropReceipt)
        *dropReceipt = DROPEFFECT_NONE;
    if (process)
        process->reset();
    if (resultFile)
        resultFile->clear();
    auto validate = [&](size_t feedbackBytes, size_t *payloadBytes = nullptr) {
        HRESULT hr = ValidateTaskPayload(t, feedbackBytes, payloadBytes);
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE))
            Report(owner,
                   L"本次操作的项目数量或路径信息超过支持的范围，请减少选择或缩短路径后重试。操作未提交。");
        else if (FAILED(hr))
            Report(owner, L"文件操作参数无效，操作未提交。", hr);
        return SUCCEEDED(hr);
    };
    if (!validate(0))
        return false;
    // 协商来源的异步完成协议；数据包只携带回执接口，不把文件内容或 UI 对象代理给工作进程。
    struct Feedback {
        ComPtr<IDataObjectAsyncCapability> async;
        ComPtr<IStream> stream;
        bool started = false, marshaled = false, committed = false;
        ~Feedback() {
            if (committed)
                return;
            if (marshaled) {
                stream->Seek({}, STREAM_SEEK_SET, nullptr);
                CoReleaseMarshalData(stream.Get());
            }
            if (started)
                async->EndOperation(E_ABORT, nullptr, DROPEFFECT_NONE);
        }
    } feedback;
    std::vector<uint8_t> packet;
    BOOL asynchronous = FALSE;
    if (dropSource && SUCCEEDED(dropSource->QueryInterface(IID_PPV_ARGS(&feedback.async))) &&
        SUCCEEDED(feedback.async->GetAsyncMode(&asynchronous)) && asynchronous) {
        HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &feedback.stream);
        if (SUCCEEDED(hr)) {
            hr = CoMarshalInterface(feedback.stream.Get(), IID_IDataObject, dropSource, MSHCTX_LOCAL, nullptr,
                                    MSHLFLAGS_NORMAL);
            feedback.marshaled = SUCCEEDED(hr);
        }
        STATSTG stat{};
        if (SUCCEEDED(hr))
            hr = feedback.stream->Stat(&stat, STATFLAG_NONAME);
        if (SUCCEEDED(hr) && (stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > MaxTaskFeedbackBytes))
            hr = E_INVALIDARG;
        if (SUCCEEDED(hr)) {
            packet.resize((size_t)stat.cbSize.QuadPart);
            feedback.stream->Seek({}, STREAM_SEEK_SET, nullptr);
            ULONG read = 0;
            hr = feedback.stream->Read(packet.data(), (ULONG)packet.size(), &read);
            if (read != packet.size())
                hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            hr = feedback.async->StartOperation(nullptr);
            feedback.started = SUCCEEDED(hr);
        }
        if (FAILED(hr)) {
            Log(L"拖放异步回执准备失败 " + ErrorText(hr));
            return false;
        }
    }
    if (dropSource && !feedback.started && !(t.allowed & DROPEFFECT_COPY)) {
        Log(L"来源仅允许移动且不支持异步回执，拒绝冒报移动完成");
        return false;
    }
    size_t payloadBytes = 0;
    if (!validate(packet.size(), &payloadBytes))
        return false;
    Writer w;
    w.bytes.reserve(payloadBytes);
    w.Pod(t.operation);
    w.Pod(t.permanent);
    w.Text(t.destination);
    w.Pod((uint32_t)t.sources.size());
    for (auto &s : t.sources)
        w.Text(s);
    w.Pod(t.keys);
    w.Pod(t.allowed);
    w.Pod(t.preferred);
    w.Pod(t.point);
    w.Pod(t.collectResults);
    w.Pod((uint32_t)packet.size());
    w.bytes.insert(w.bytes.end(), packet.begin(), packet.end());
    GUID id{};
    HRESULT hr = CoCreateGuid(&id);
    wchar_t token[40]{};
    if (FAILED(hr) || !StringFromGUID2(id, token, 40)) {
        Report(owner, L"无法生成文件任务标识", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    auto path = DataDir() / L"tasks" / (std::wstring(token) + L".task");
    if (!WriteEnvelope(path, TaskMagic, w.bytes, false)) {
        Report(owner, L"无法保存文件操作任务");
        return false;
    }
    wchar_t currentExe[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, currentExe, 32768);
    if (!length || length >= 32768) {
        DWORD error = length ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        DeleteFileW(path.c_str());
        Report(owner, L"无法获取操作程序路径", HRESULT_FROM_WIN32(error));
        return false;
    }
    auto exe = executable.empty() ? std::filesystem::path(currentExe) : executable;
    std::wstring cmd = Quote(exe.wstring()) + L" --task " + Quote(path.wstring());
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr,
                        ExeDir().c_str(), &si, &pi)) {
        DWORD e = GetLastError();
        DeleteFileW(path.c_str());
        Report(owner, L"无法启动独立文件操作", HRESULT_FROM_WIN32(e));
        return false;
    }
    CloseHandle(pi.hThread);
    feedback.committed = true;
    if (dropReceipt)
        *dropReceipt = feedback.started ? t.preferred : DROPEFFECT_COPY;
    if (process)
        *process = std::shared_ptr<void>(pi.hProcess, [](void *handle) { CloseHandle(handle); });
    else
        CloseHandle(pi.hProcess);
    if (resultFile) {
        *resultFile = path;
        *resultFile += L".result";
    }
    Log(L"文件任务已交付 pid=" + std::to_wstring(pi.dwProcessId) + L" asyncFeedback=" +
        std::to_wstring(feedback.started) + L" " + path.filename().wstring());
    return true;
}
std::vector<std::wstring> DataItems(IDataObject *object) {
    std::vector<std::wstring> out;
    ComPtr<IShellItemArray> arr;
    if (!object || FAILED(SHCreateShellItemArrayFromDataObject(object, IID_PPV_ARGS(&arr))))
        return out;
    DWORD n = 0;
    arr->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> item;
        arr->GetItemAt(i, &item);
        auto name = ItemName(item.Get());
        if (name.empty())
            return {};
        out.push_back(name);
    }
    return out;
}
std::optional<DWORD> DataEffect(IDataObject *o, const wchar_t *format) {
    FORMATETC f{(CLIPFORMAT)RegisterClipboardFormatW(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM s{};
    std::optional<DWORD> effect;
    if (o && SUCCEEDED(o->GetData(&f, &s))) {
        if (s.tymed == TYMED_HGLOBAL && s.hGlobal && GlobalSize(s.hGlobal) >= sizeof(DWORD))
            if (auto *p = (DWORD *)GlobalLock(s.hGlobal)) {
                effect = *p;
                GlobalUnlock(s.hGlobal);
            }
        ReleaseStgMedium(&s);
    }
    return effect;
}
DWORD PreferredEffect(IDataObject *o) {
    return DataEffect(o, CFSTR_PREFERREDDROPEFFECT).value_or(DROPEFFECT_COPY);
}
HRESULT SetDataEffect(IDataObject *object, const wchar_t *format, DWORD effect) {
    if (!object)
        return E_POINTER;
    FORMATETC f{(CLIPFORMAT)RegisterClipboardFormatW(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!medium.hGlobal)
        return E_OUTOFMEMORY;
    auto *value = static_cast<DWORD *>(GlobalLock(medium.hGlobal));
    if (!value) {
        ReleaseStgMedium(&medium);
        return E_OUTOFMEMORY;
    }
    *value = effect;
    GlobalUnlock(medium.hGlobal);
    HRESULT hr = object->SetData(&f, &medium, TRUE);
    if (FAILED(hr))
        ReleaseStgMedium(&medium);
    return hr;
}
bool SnapshotFiles(IDataObject *object, std::vector<std::wstring> &paths) {
    paths.clear();
    auto reject = [](const std::wstring &reason, HRESULT hr = S_OK) {
        Log(L"文件快照回退原因：" + reason + (FAILED(hr) ? L" " + ErrorText(hr) : L""));
        return false;
    };
    if (!object)
        return reject(L"没有数据对象");
    // Shell 文件数据可以同时提供文件描述格式；实际文件系统项目才是可重建的依据。
    ComPtr<IShellItemArray> items;
    DWORD count = 0;
    HRESULT hr = SHCreateShellItemArrayFromDataObject(object, IID_PPV_ARGS(&items));
    if (FAILED(hr))
        return reject(L"无法取得 Shell 项目数组", hr);
    hr = items->GetCount(&count);
    if (FAILED(hr) || !count || count > MaxTaskItems)
        return reject(L"项目数量无效", hr);
    std::vector<std::wstring> snapshot;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        SFGAOF flags = 0;
        hr = items->GetItemAt(i, &item);
        if (SUCCEEDED(hr))
            hr = item->GetAttributes(SFGAO_FILESYSTEM, &flags);
        if (FAILED(hr) || !(flags & SFGAO_FILESYSTEM))
            return reject(L"来源不是可重建的文件系统项目 index=" + std::to_wstring(i), hr);
        auto path = ItemName(item.Get(), SIGDN_FILESYSPATH);
        if (path.empty())
            return reject(L"来源没有文件系统路径 index=" + std::to_wstring(i));
        snapshot.push_back(std::move(path));
    }
    // QueryGetData 只表示支持格式，不表示存在改名要求。检查实际映射，
    // 空映射或与源名一致的映射不应阻止普通文件进入独立进程。
    for (bool wide : {true, false}) {
        FORMATETC format{(CLIPFORMAT)RegisterClipboardFormatW(wide ? CFSTR_FILENAMEMAPW : L"FileNameMap"),
                         nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        if (object->QueryGetData(&format) != S_OK)
            continue;
        STGMEDIUM medium{};
        hr = object->GetData(&format, &medium);
        if (hr == DV_E_FORMATETC || hr == DV_E_TYMED || hr == E_NOTIMPL)
            continue;
        if (FAILED(hr))
            return reject(L"无法读取文件名映射", hr);
        struct ReleaseMedium {
            STGMEDIUM &value;
            ~ReleaseMedium() {
                ReleaseStgMedium(&value);
            }
        } release{medium};
        if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal)
            return reject(L"文件名映射介质无效");
        SIZE_T bytes = GlobalSize(medium.hGlobal);
        if (!bytes)
            continue;
        if (bytes > 16 * 1024 * 1024 || (wide && bytes % sizeof(wchar_t)))
            return reject(L"文件名映射长度无效");
        auto *raw = GlobalLock(medium.hGlobal);
        if (!raw)
            return reject(L"无法锁定文件名映射");
        struct Unlock {
            HGLOBAL handle;
            ~Unlock() {
                GlobalUnlock(handle);
            }
        } unlock{medium.hGlobal};
        size_t units = wide ? bytes / sizeof(wchar_t) : bytes;
        auto zero = [&](size_t i) {
            return wide ? static_cast<const wchar_t *>(raw)[i] == 0 : static_cast<const char *>(raw)[i] == 0;
        };
        size_t pos = 0, index = 0;
        while (pos < units && !zero(pos)) {
            size_t end = pos;
            while (end < units && !zero(end))
                ++end;
            if (end == units || index >= snapshot.size())
                return reject(L"文件名映射未终止或数量不符");
            std::wstring name;
            if (wide)
                name.assign(static_cast<const wchar_t *>(raw) + pos, end - pos);
            else {
                auto *start = static_cast<const char *>(raw) + pos;
                int length = MultiByteToWideChar(CP_ACP, 0, start, (int)(end - pos), nullptr, 0);
                if (!length)
                    return reject(L"文件名映射编码无效");
                name.resize(length);
                MultiByteToWideChar(CP_ACP, 0, start, (int)(end - pos), name.data(), length);
            }
            const auto &source = snapshot[index++];
            if (_wcsicmp(name.c_str(), source.c_str()) &&
                _wcsicmp(name.c_str(), PathFindFileNameW(source.c_str())))
                return reject(L"来源包含实际改名映射");
            pos = end + 1;
        }
        if (pos == units || (index && index != snapshot.size()))
            return reject(L"文件名映射未终止或数量不符");
    }
    paths = std::move(snapshot);
    return true;
}
HRESULT TaskShellItems(const std::vector<std::wstring> &paths, IShellItemArray **items) {
    if (!items)
        return E_POINTER;
    *items = nullptr;
    if (paths.empty() || paths.size() > MaxTaskItems)
        return E_INVALIDARG;
    struct Lists {
        std::vector<PCIDLIST_ABSOLUTE> values;
        ~Lists() {
            for (auto p : values)
                CoTaskMemFree(const_cast<PIDLIST_ABSOLUTE>(p));
        }
    } lists;
    for (const auto &path : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
        if (FAILED(hr))
            return hr;
        lists.values.push_back(pidl);
    }
    return SHCreateShellItemArrayFromIDLists((UINT)lists.values.size(), lists.values.data(), items);
}
HRESULT SystemDropTarget(IShellItem *destination, IDropTarget **target) {
    if (!target)
        return E_POINTER;
    *target = nullptr;
    return destination ? destination->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(target))
                       : E_INVALIDARG;
}
} // namespace zen
