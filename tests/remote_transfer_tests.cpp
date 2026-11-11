#include "native_transfer.h"
#include <wrl/implements.h>
#include <fstream>
#include <iostream>
#include <cstring>
using namespace zen;

static void Require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
class RemoteData final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IDataObject> {
  public:
    std::vector<FILEDESCRIPTORW> descriptors;
    std::vector<std::string> content;
    bool memory = false, malformed = false;
    HANDLE submitter = nullptr;
    bool observedExit = false;
    void Add(const wchar_t *name, std::string bytes, bool directory = false) {
        FILEDESCRIPTORW descriptor{};
        descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE;
        descriptor.dwFileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        descriptor.nFileSizeLow = static_cast<DWORD>(bytes.size());
        wcscpy_s(descriptor.cFileName, name);
        descriptors.push_back(descriptor);
        content.push_back(std::move(bytes));
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *f, STGMEDIUM *s) override {
        if (!f || !s)
            return E_POINTER;
        *s = {};
        if (f->cfFormat == RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)) {
            SIZE_T size = offsetof(FILEGROUPDESCRIPTORW, fgd) + descriptors.size() * sizeof(FILEDESCRIPTORW);
            s->tymed = TYMED_HGLOBAL;
            s->hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
            if (!s->hGlobal)
                return E_OUTOFMEMORY;
            auto *data = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(s->hGlobal));
            data->cItems = malformed ? MaxTaskItems + 1 : static_cast<UINT>(descriptors.size());
            memcpy(data->fgd, descriptors.data(), descriptors.size() * sizeof(FILEDESCRIPTORW));
            GlobalUnlock(s->hGlobal);
            return S_OK;
        }
        if (f->cfFormat == RegisterClipboardFormatW(CFSTR_FILECONTENTS) && f->lindex >= 0 &&
            static_cast<size_t>(f->lindex) < content.size()) {
            // 来源进程保持运行，提交者在启动工作进程后退出；内容随后才提供给工作进程。
            if (submitter) {
                observedExit = WaitForSingleObject(submitter, 5000) == WAIT_OBJECT_0;
                if (!observedExit)
                    return E_FAIL;
            }
            auto &bytes = content[f->lindex];
            if (memory) {
                s->tymed = TYMED_HGLOBAL;
                s->hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, std::max<size_t>(1, bytes.size()));
                auto *p = GlobalLock(s->hGlobal);
                memcpy(p, bytes.data(), bytes.size());
                GlobalUnlock(s->hGlobal);
            } else {
                s->tymed = TYMED_ISTREAM;
                s->pstm = SHCreateMemStream(reinterpret_cast<const BYTE *>(bytes.data()),
                                            static_cast<UINT>(bytes.size()));
            }
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *f) override {
        return f && (f->cfFormat == RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW) ||
                     f->cfFormat == RegisterClipboardFormatW(CFSTR_FILECONTENTS))
                   ? S_OK
                   : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override {
        FORMATETC formats[]{{(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW), nullptr,
                             DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                            {(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS), nullptr,
                             DVASPECT_CONTENT, -1, TYMED_ISTREAM | TYMED_HGLOBAL}};
        return direction == DATADIR_GET ? SHCreateStdEnumFmtEtc(2, formats, out) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
};

// 直接调用正式原生接收入口，不读取或替换用户剪贴板。
static void NativeReception(const std::filesystem::path &root) {
    bool submitted = true;
    Require(NativePaste(nullptr, nullptr, &submitted) == E_INVALIDARG && !submitted,
            "invalid arguments remain caller-owned errors");
    auto data = Microsoft::WRL::Make<RemoteData>();
    data->Add(L"目录", {}, true);
    data->Add(L"目录\\payload.bin", std::string(1024 * 1024 + 17, 'r'));
    data->Add(L"目录\\空目录", {}, true);
    data->Add(L"zero.txt", {});
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto destination = root / (L"native-" + std::to_wstring(attempt));
        std::filesystem::create_directories(destination);
        if (attempt == 2)
            data->content[1].assign(data->content[1].size(), 's');
        auto target = ParseItem(destination.wstring());
        submitted = false;
        Require(SUCCEEDED(NativePaste(data.Get(), target.Get(), &submitted)) && submitted,
                "native target owns submitted transfer");
        std::ifstream file(destination / L"目录" / L"payload.bin", std::ios::binary);
        std::string actual((std::istreambuf_iterator<char>(file)), {});
        Require(actual == data->content[1], "native content matches current source");
        Require(std::filesystem::is_empty(destination / L"目录" / L"空目录"), "empty directory");
        Require(std::filesystem::file_size(destination / L"zero.txt") == 0, "empty file");
        for (const auto &entry : std::filesystem::recursive_directory_iterator(destination))
            Require(entry.path().filename().wstring().find(L".zen-receive-") != 0,
                    "no private staging directory");
    }
}

int wmain(int argc, wchar_t **argv) {
    OleInitialize(nullptr);
    if (argc == 4 && std::wstring(argv[1]) == L"--submit-paste") {
        FileTask task{Operation::Paste, argv[2]};
        task.clipboardSequence = std::stoul(argv[3]);
        bool ok = LaunchTask(nullptr, task, ExeDir() / L"ZenExplorer.exe");
        OleUninitialize();
        return ok ? 0 : 1;
    }
    ComPtr<IDataObject> original;
    DWORD testSequence = 0;
    auto restore = [&] {
        if (testSequence && GetClipboardSequenceNumber() == testSequence) {
            OleSetClipboard(original.Get());
            if (original)
                OleFlushClipboard();
        }
    };
    try {
        auto root =
            std::filesystem::current_path() / L".test-data" /
            (L"remote-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root / L"destination");
        SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", root.c_str());
        NativeReception(root);
        if (argc == 2 && std::wstring(argv[1]) == L"--unit-only") {
            original.Reset();
            OleUninitialize();
            std::cout << "native Shell reception passed without accessing clipboard\n";
            return 0;
        }
        OleGetClipboard(&original);
        auto data = Microsoft::WRL::Make<RemoteData>();
        data->Add(L"远程目录", {}, true);
        data->Add(L"远程目录\\多级", {}, true);
        data->Add(L"远程目录\\多级\\payload.bin", std::string(2 * 1024 * 1024 + 11, 'v'));
        data->Add(L"远程目录\\空目录", {}, true);
        data->Add(L"zero.txt", {});
        Require(SUCCEEDED(OleSetClipboard(data.Get())), "install simulated remote source");
        testSequence = GetClipboardSequenceNumber();
        wchar_t self[32768]{};
        GetModuleFileNameW(nullptr, self, 32768);
        std::wstring command = Quote(self) + L" --submit-paste " + Quote((root / L"destination").wstring()) +
                               L" " + std::to_wstring(testSequence);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                               nullptr, &startup, &process) != FALSE,
                "create paste submitter");
        CloseHandle(process.hThread);
        data->submitter = process.hProcess;
        auto deadline = GetTickCount64() + 25000;
        std::optional<TaskResult> completed;
        while (!completed && GetTickCount64() < deadline) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            auto tasks = DataDir() / L"tasks";
            if (std::filesystem::exists(tasks))
                for (const auto &file : std::filesystem::directory_iterator(tasks))
                    if (file.path().extension() == L".result")
                        completed = ReadTaskResult(file.path());
            Sleep(10);
        }
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        data->submitter = nullptr;
        Require(completed && SUCCEEDED(completed->invoked) && exitCode == 0 && data->observedExit,
                "worker copy completes after submitter exits");
        auto output = root / L"destination" / L"远程目录" / L"多级" / L"payload.bin";
        std::ifstream file(output, std::ios::binary);
        std::string actual((std::istreambuf_iterator<char>(file)), {});
        Require(actual == data->content[2], "independent virtual copy contents");
        Require(std::filesystem::is_directory(root / L"destination" / L"远程目录" / L"空目录"),
                "empty directory retained");
        Require(std::filesystem::file_size(root / L"destination" / L"zero.txt") == 0, "zero byte file");
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root / L"destination"))
            Require(entry.path().filename().wstring().find(L".zen-receive-") != 0, "temporary file cleaned");
        restore();
        original.Reset();
        std::cout
            << "native reception and independent paste passed\n";
        OleUninitialize();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        restore();
        original.Reset();
        OleUninitialize();
        return 1;
    }
}
