#include "platform.h"
#include <commoncontrols.h>
#include <iomanip>
#include <sstream>
namespace zen {
std::filesystem::path ExeDir() {
    wchar_t b[32768]{};
    GetModuleFileNameW(nullptr, b, 32768);
    return std::filesystem::path(b).parent_path();
}
// 测试可覆盖整个便携目录，生产默认使用 EXE 所在目录，不依赖工作目录。
static std::filesystem::path PortableRoot() {
    wchar_t env[32768]{};
    DWORD length = GetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", env, 32768);
    if (length && length < 32768)
        return env;
    return ExeDir();
}
std::filesystem::path DataDir() {
    return PortableRoot() / L"data";
}
std::filesystem::path ConfigDir() {
    return PortableRoot() / L"config";
}
std::filesystem::path LogDir() {
    return PortableRoot() / L"log";
}
bool FileLogEnabled() {
    wchar_t enabled[2]{};
    return GetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", enabled, 2) == 1 && enabled[0] == L'1';
}
namespace {
constexpr LONGLONG MaxLogBytes = 4 * 1024 * 1024;
constexpr size_t MaxLogRecordBytes = 64 * 1024;
struct LogHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~LogHandle() {
        Reset();
    }
    void Reset(HANDLE next = INVALID_HANDLE_VALUE) {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = next;
    }
};
bool MissingLogFile() {
    return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
}
bool TrimOversizedArchive(const std::filesystem::path &file) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info))
        return MissingLogFile();
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return false;
    ULARGE_INTEGER size{};
    size.HighPart = info.nFileSizeHigh;
    size.LowPart = info.nFileSizeLow;
    // 旧版本可能生成无限增长的日志；不把超限旧文件继续带入保留档。
    return size.QuadPart <= MaxLogBytes || DeleteFileW(file.c_str());
}
std::string LogRecord(const std::wstring &s) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t prefix[80];
    swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, GetCurrentProcessId());
    size_t length = std::min(s.size(), MaxLogRecordBytes);
    // 先限制 UTF-16 输入，避免为了截断一条诊断消息又分配同等大小的副本。
    if (length < s.size() && length && s[length - 1] >= 0xd800 && s[length - 1] <= 0xdbff)
        --length;
    auto message = std::wstring(prefix) + s.substr(0, length);
    int bytes =
        WideCharToMultiByte(CP_UTF8, 0, message.data(), (int)message.size(), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string record(bytes, 0);
    if (!WideCharToMultiByte(CP_UTF8, 0, message.data(), (int)message.size(), record.data(), bytes, nullptr,
                             nullptr))
        return {};
    if (length < s.size() || record.size() + 2 > MaxLogRecordBytes) {
        constexpr char suffix[] = " [truncated]";
        size_t end = std::min(record.size(), MaxLogRecordBytes - (sizeof(suffix) - 1) - 2);
        while (end < record.size() && (static_cast<unsigned char>(record[end]) & 0xc0) == 0x80)
            --end;
        record.resize(end);
        record += suffix;
    }
    return record + "\r\n";
}
void AppendLog(const std::string &record) {
    if (record.empty())
        return;
    auto folder = LogDir();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec)
        return;
    // 同一目录的主进程、工作进程及不同会话共用文件锁；锁忙即丢弃本条，不等待。
    LogHandle lock{CreateFileW((folder / L"application.log.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr)};
    OVERLAPPED range{};
    if (lock.value == INVALID_HANDLE_VALUE ||
        !LockFileEx(lock.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range))
        return;
    // 关闭锁文件句柄即释放字节锁；锁文件不能在运行中删除，否则其他进程可能锁住不同文件。
    const auto file = folder / L"application.log";
    const auto older = folder / L"application.log.1";
    const auto oldest = folder / L"application.log.2";
    if (!TrimOversizedArchive(older) || !TrimOversizedArchive(oldest))
        return;
    LogHandle active{CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr)};
    LARGE_INTEGER size{};
    if (active.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(active.value, &size))
        return;
    if (size.QuadPart > MaxLogBytes - (LONGLONG)record.size()) {
        active.Reset();
        // 先取得当前文件的删除访问权，读者禁止轮转时不提前覆盖旧档。
        active.Reset(CreateFileW(file.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (active.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(active.value, &size))
            return;
        if (size.QuadPart > MaxLogBytes) {
            if (!DeleteFileW(file.c_str()))
                return;
        } else {
            if (!MoveFileExW(older.c_str(), oldest.c_str(), MOVEFILE_REPLACE_EXISTING) && !MissingLogFile())
                return;
            if (!MoveFileExW(file.c_str(), older.c_str(), MOVEFILE_REPLACE_EXISTING))
                return;
        }
        active.Reset();
        active.Reset(CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
        if (active.value == INVALID_HANDLE_VALUE)
            return;
        size.QuadPart = 0;
    }
    if (!SetFilePointerEx(active.value, size, nullptr, FILE_BEGIN))
        return;
    DWORD written = 0;
    if (!WriteFile(active.value, record.data(), (DWORD)record.size(), &written, nullptr) ||
        written != record.size()) {
        // 尽力撤回短写，避免下一条记录接在半条 UTF-8 内容后面；失败不递归记录日志。
        if (SetFilePointerEx(active.value, size, nullptr, FILE_BEGIN))
            SetEndOfFile(active.value);
    }
}
} // namespace
void Log(const std::wstring &s) noexcept {
    const DWORD previousError = GetLastError();
    try {
        // 默认不创建目录或文件；诊断时显式设置为 1，子任务继承该开关。
        if (FileLogEnabled())
            AppendLog(LogRecord(s));
    } catch (...) {
        // 诊断失败（包括内存分配失败）不能中断原业务。
    }
    SetLastError(previousError);
}
ResourceTrace::ResourceTrace(std::wstring name) : phase(std::move(name)) {
    enabled = FileLogEnabled();
    if (!enabled)
        return;
    start = GetTickCount64();
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    Log(phase + L" 开始 handles=" + std::to_wstring(handles) + L" gdi=" + std::to_wstring(gdi) + L" user=" +
        std::to_wstring(user));
}
ResourceTrace::~ResourceTrace() {
    if (!enabled)
        return;
    DWORD endHandles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &endHandles);
    auto endGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    auto endUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    Log(phase + L" 结束 ms=" + std::to_wstring(GetTickCount64() - start) + L" handles=" +
        std::to_wstring(endHandles) + L" gdi=" + std::to_wstring(endGdi) + L" user=" +
        std::to_wstring(endUser) + L" delta=" + std::to_wstring((long long)endHandles - handles) + L"," +
        std::to_wstring((long long)endGdi - gdi) + L"," + std::to_wstring((long long)endUser - user));
}
std::wstring ErrorText(HRESULT hr) {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, hr, 0, (wchar_t *)&text, 0, nullptr);
    std::wstring out = text ? text : L"系统操作失败";
    LocalFree(text);
    wchar_t code[32];
    swprintf_s(code, L" (0x%08X)", (unsigned)hr);
    return out + code;
}
std::wstring ItemName(IShellItem *item, SIGDN type) {
    PWSTR p = nullptr;
    if (!item || FAILED(item->GetDisplayName(type, &p)))
        return {};
    std::wstring s = p;
    CoTaskMemFree(p);
    return s;
}
ComPtr<IShellItem> ParseItem(const std::wstring &path) {
    ComPtr<IShellItem> p;
    SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&p));
    return p;
}
bool CanNavigateUp(const std::wstring &path) {
    return !path.empty() && _wcsicmp(path.c_str(), Home) != 0 &&
           _wcsicmp(path.c_str(), L"::{00021400-0000-0000-C000-000000000046}") != 0;
}
std::wstring LocationName(const std::wstring &path) {
    if (path == Home)
        return L"此电脑";
    if (path == L"::{00021400-0000-0000-C000-000000000046}")
        return L"桌面";
    auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos || pos + 1 == path.size() ? path : path.substr(pos + 1);
}
bool IsAbsoluteDrivePath(const std::wstring &path) {
    return path.size() >= 3 &&
           ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
           path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}
std::wstring SizeText(uint64_t bytes) {
    static const wchar_t *units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double n = (double)bytes;
    int u = 0;
    while (n >= 1024 && u < 5) {
        n /= 1024;
        ++u;
    }
    std::wostringstream s;
    s << std::fixed << std::setprecision(u ? 2 : 0) << n << L" " << units[u];
    return s.str();
}
std::wstring Quote(const std::wstring &s) {
    std::wstring out = L"\"";
    unsigned slash = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++slash;
            continue;
        }
        if (c == L'"')
            out.append(slash * 2 + 1, L'\\');
        else
            out.append(slash, L'\\');
        slash = 0;
        out += c;
    }
    out.append(slash * 2, L'\\');
    return out + L"\"";
}
std::wstring WindowText(HWND w) {
    int n = GetWindowTextLengthW(w);
    std::wstring s(n + 1, 0);
    GetWindowTextW(w, s.data(), n + 1);
    s.resize(n);
    return s;
}
void Report(HWND w, const std::wstring &s, HRESULT hr) {
    Log(s + (FAILED(hr) ? L": " + ErrorText(hr) : L""));
    MessageBoxW(w, (s + (FAILED(hr) ? L"\n" + ErrorText(hr) : L"")).c_str(), L"ZenExplorer",
                MB_OK | MB_ICONERROR);
}
// 不访问 UNC 或映射网络驱动器；只有在线本地卷上的路径不存在才显示失效。
LocalPathState InspectLocalPath(const std::wstring &path) {
    if (!IsAbsoluteDrivePath(path))
        return LocalPathState::Deferred;
    auto root = path.substr(0, 2) + L"\\";
    UINT type = GetDriveTypeW(root.c_str());
    if (type == DRIVE_REMOTE || type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR)
        return LocalPathState::Deferred;
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES)
        return LocalPathState::Exists;
    DWORD error = GetLastError();
    if ((error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) &&
        GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES)
        return LocalPathState::Missing;
    return LocalPathState::Unknown;
}
// 使用 Windows 10 自带 Segoe MDL2 Assets，不下载或分发字体文件。
void DrawSystemGlyph(HDC dc, RECT rect, wchar_t glyph, UINT dpi, COLORREF color) {
    HFONT font = CreateFontW(-MulDiv(16, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                             L"Segoe MDL2 Assets");
    if (!font)
        return;
    int saved = SaveDC(dc);
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, &glyph, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(dc, saved);
    DeleteObject(font);
}
std::vector<std::pair<std::wstring, std::wstring>> Breadcrumbs(const std::wstring &input) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    auto path = input;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"::"))
        return {{LocationName(path), path}};
    bool drive = IsAbsoluteDrivePath(path);
    bool unc = path.starts_with(L"\\\\");
    if (!drive && !unc)
        return {{LocationName(path), path}};
    out.push_back({L"此电脑", Home});
    size_t start = unc ? 2 : 0;
    for (size_t pos = path.find(L'\\', start);; pos = path.find(L'\\', pos + 1)) {
        size_t end = pos == std::wstring::npos ? path.size() : pos;
        auto full = path.substr(0, end);
        if (full.size() == 2 && full[1] == L':')
            full += L"\\";
        if (end > start)
            out.push_back({path.substr(start, end - start), full});
        start = end + 1;
        if (pos == std::wstring::npos)
            break;
    }
    return out;
}
bool ValidPidl(const std::vector<BYTE> &bytes) {
    if (bytes.size() < sizeof(USHORT) || bytes.size() > 1024 * 1024)
        return false;
    size_t offset = 0;
    while (offset + sizeof(USHORT) <= bytes.size()) {
        USHORT size = 0;
        memcpy(&size, bytes.data() + offset, sizeof(size));
        if (!size)
            return offset + sizeof(size) == bytes.size();
        if (size < sizeof(size) || size > bytes.size() - offset)
            return false;
        offset += size;
    }
    return false;
}
HRESULT ShellHierarchy(IShellItem *item, std::vector<ComPtr<IShellItem>> &out) {
    out.clear();
    if (!item)
        return E_INVALIDARG;
    struct Ids {
        PIDLIST_ABSOLUTE chain = nullptr, root = nullptr;
        ~Ids() {
            CoTaskMemFree(chain);
            CoTaskMemFree(root);
        }
    } ids;
    HRESULT hr = SHGetIDListFromObject(item, &ids.chain);
    if (FAILED(hr) || !ids.chain)
        return FAILED(hr) ? hr : E_FAIL;
    SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &ids.root);
    ComPtr<IShellItem> current = item;
    for (size_t depth = 0; depth < 256; ++depth) {
        out.push_back(current);
        if (ILIsEmpty(ids.chain) || (ids.root && ILIsEqual(ids.chain, ids.root)))
            return S_OK;
        if (!ILRemoveLastID(ids.chain))
            return E_FAIL;
        current.Reset();
        hr = SHCreateItemFromIDList(ids.chain, IID_PPV_ARGS(&current));
        if (FAILED(hr))
            return hr;
    }
    return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
}
HRESULT ChildFolders(const std::wstring &path, std::vector<std::pair<std::wstring, std::wstring>> &out) {
    out.clear();
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr))
        return hr;
    std::vector<ComPtr<IShellItem>> children;
    hr = ChildFolderItems(item.Get(), children);
    if (FAILED(hr))
        return hr;
    for (const auto &child : children) {
        auto name = ItemName(child.Get(), SIGDN_NORMALDISPLAY);
        auto target = ItemName(child.Get());
        if (name.empty() || target.empty()) {
            out.clear();
            return E_FAIL;
        }
        out.emplace_back(std::move(name), std::move(target));
    }
    return S_OK;
}
HRESULT ChildFolderItems(IShellItem *item, std::vector<ComPtr<IShellItem>> &out) {
    out.clear();
    if (!item)
        return E_INVALIDARG;
    ComPtr<IShellFolder> folder;
    HRESULT hr = item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder));
    ComPtr<IEnumIDList> entries;
    if (SUCCEEDED(hr))
        hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS, &entries);
    if (FAILED(hr))
        return hr;
    if (!entries)
        return S_OK;
    std::vector<std::pair<std::wstring, ComPtr<IShellItem>>> result;
    for (;;) {
        PITEMID_CHILD child = nullptr;
        hr = entries->Next(1, &child, nullptr);
        if (hr != S_OK) {
            CoTaskMemFree(child);
            break;
        }
        ComPtr<IShellItem> entry;
        hr = SHCreateItemWithParent(nullptr, folder.Get(), child, IID_PPV_ARGS(&entry));
        CoTaskMemFree(child);
        if (FAILED(hr))
            return hr;
        auto name = ItemName(entry.Get(), SIGDN_NORMALDISPLAY);
        if (name.empty())
            return E_FAIL;
        result.emplace_back(std::move(name), std::move(entry));
    }
    if (FAILED(hr))
        return hr;
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return StrCmpLogicalW(a.first.c_str(), b.first.c_str()) < 0;
    });
    for (auto &[name, child] : result)
        out.push_back(std::move(child));
    return S_OK;
}
bool Confirm(HWND w, const std::wstring &text) {
    TASKDIALOG_BUTTON buttons[] = {{IDOK, L"确认"}, {IDCANCEL, L"取消"}};
    TASKDIALOGCONFIG c{sizeof(c)};
    c.hwndParent = w;
    c.pszWindowTitle = L"ZenExplorer";
    c.pszContent = text.c_str();
    c.pButtons = buttons;
    c.cButtons = 2;
    c.nDefaultButton = IDCANCEL;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    int selected = 0;
    return SUCCEEDED(TaskDialogIndirect(&c, &selected, nullptr, nullptr)) && selected == IDOK;
}
HICON SystemImageIcon(int index, int size) {
    if (index < 0 || size <= 0)
        return nullptr;
    // 从足够大的系统图像列表取图，再缩小到目标像素尺寸，不放大小图。
    for (int kind : {SHIL_SMALL, SHIL_LARGE, SHIL_EXTRALARGE, SHIL_JUMBO}) {
        ComPtr<IImageList> list;
        if (FAILED(SHGetImageList(kind, IID_PPV_ARGS(&list))))
            continue;
        int width = 0, height = 0;
        if (FAILED(list->GetIconSize(&width, &height)) || width < size || height < size)
            continue;
        HICON original = nullptr;
        if (FAILED(list->GetIcon(index, ILD_TRANSPARENT, &original)))
            continue;
        HICON result = (HICON)CopyImage(original, IMAGE_ICON, size, size, 0);
        DestroyIcon(original);
        return result;
    }
    return nullptr;
}
struct Prompt {
    HWND edit{};
    HFONT font{};
    UINT dpi = 96;
    bool done = false, ok = false;
    std::wstring value, hint;
};
static void LayoutPrompt(HWND w, Prompt &p) {
    auto scale = [&](int v) { return MulDiv(v, p.dpi, 96); };
    HFONT next = CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    if (next) {
        for (HWND child = GetWindow(w, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, (WPARAM)next, TRUE);
        if (p.font)
            DeleteObject(p.font);
        p.font = next;
    }
    MoveWindow(p.edit, scale(16), scale(20), scale(380), scale(28), TRUE);
    int y = p.hint.empty() ? 65 : 125;
    MoveWindow(GetDlgItem(w, IDOK), scale(215), scale(y), scale(85), scale(30), TRUE);
    MoveWindow(GetDlgItem(w, IDCANCEL), scale(310), scale(y), scale(85), scale(30), TRUE);
    if (!p.hint.empty())
        MoveWindow(GetDlgItem(w, 102), scale(16), scale(58), scale(380), scale(62), TRUE);
}
static LRESULT CALLBACK PromptProc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto *p = (Prompt *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        p = (Prompt *)((CREATESTRUCTW *)b)->lpCreateParams;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)p);
    }
    if (m == WM_CREATE) {
        p->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", p->value.c_str(),
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 16, 20, 380, 28, w,
                                  (HMENU)101, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"确认", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 215,
                      p->hint.empty() ? 65 : 125, 85, 30, w, (HMENU)IDOK, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 310, p->hint.empty() ? 65 : 125,
                      85, 30, w, (HMENU)IDCANCEL, nullptr, nullptr);
        if (!p->hint.empty())
            CreateWindowW(L"STATIC", p->hint.c_str(), WS_CHILD | WS_VISIBLE, 16, 58, 380, 62, w, (HMENU)102,
                          nullptr, nullptr);
        p->dpi = GetDpiForWindow(w);
        LayoutPrompt(w, *p);
        SendMessageW(p->edit, EM_SETSEL, 0, -1);
        SetFocus(p->edit);
        return 0;
    }
    if (m == WM_DPICHANGED && p) {
        p->dpi = HIWORD(a);
        const auto &r = *(RECT *)b;
        SetWindowPos(w, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutPrompt(w, *p);
        return 0;
    }
    if (m == WM_NCDESTROY && p && p->font) {
        DeleteObject(p->font);
        p->font = nullptr;
    }
    if (m == WM_COMMAND && (LOWORD(a) == IDOK || LOWORD(a) == IDCANCEL)) {
        p->ok = LOWORD(a) == IDOK;
        p->value = WindowText(p->edit);
        p->done = true;
        return 0;
    }
    if (m == WM_CLOSE) {
        p->done = true;
        return 0;
    }
    return DefWindowProcW(w, m, a, b);
}
std::optional<std::wstring> Input(HWND owner, const std::wstring &title, const std::wstring &value,
                                  const std::wstring &hint) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = PromptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ZenPrompt";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    Prompt p;
    p.value = value;
    p.hint = hint;
    RECT r{};
    GetWindowRect(owner, &r);
    p.dpi = GetDpiForWindow(owner);
    if (!p.dpi)
        p.dpi = 96;
    RECT bounds{0, 0, MulDiv(412, p.dpi, 96), MulDiv(hint.empty() ? 110 : 170, p.dpi, 96)};
    AdjustWindowRectExForDpi(&bounds, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE, WS_EX_DLGMODALFRAME, p.dpi);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ZenPrompt", title.c_str(),
                             WS_CAPTION | WS_SYSMENU | WS_POPUP, r.left + MulDiv(40, p.dpi, 96),
                             r.top + MulDiv(40, p.dpi, 96), bounds.right - bounds.left,
                             bounds.bottom - bounds.top, owner, nullptr, wc.hInstance, &p);
    if (!w)
        return {};
    EnableWindow(owner, FALSE);
    ShowWindow(w, SW_SHOW);
    MSG msg{};
    while (!p.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            SendMessageW(w, WM_COMMAND, msg.wParam == VK_RETURN ? IDOK : IDCANCEL, 0);
            continue;
        }
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(w);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (p.ok)
        return p.value;
    return {};
}
} // namespace zen
