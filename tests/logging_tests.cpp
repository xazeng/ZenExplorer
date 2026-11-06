#include "platform.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace zen;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)

namespace {
constexpr size_t LogLimit = 4 * 1024 * 1024;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
std::filesystem::path testRoot;
void Case(const wchar_t *name) {
    auto root = testRoot / name;
    CHECK(SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", root.c_str()));
    CHECK(SetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", L"1"));
}
std::string Read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size());
    CHECK(output.good());
}
void CheckUtf8(const std::string &bytes) {
    CHECK(!bytes.empty());
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0) >
          0);
    CHECK(bytes.ends_with("\r\n"));
}
void CheckDisabledAndEncoding() {
    Case(L"encoding");
    CHECK(SetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", nullptr));
    SetLastError(ERROR_BAD_ARGUMENTS);
    Log(L"默认关闭");
    CHECK(GetLastError() == ERROR_BAD_ARGUMENTS && !std::filesystem::exists(LogDir()));
    CHECK(SetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", L"10"));
    Log(L"不是启用值");
    CHECK(!std::filesystem::exists(LogDir()));
    CHECK(SetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", L"1"));
    Log(L"中文日志 \U0001f600");
    auto first = Read(LogDir() / L"application.log");
    CheckUtf8(first);
    CHECK(first.find("[truncated]") == std::string::npos);
    std::wstring large;
    for (int i = 0; i < 50000; ++i)
        large += L"中\U0001f600";
    Log(large);
    auto all = Read(LogDir() / L"application.log");
    CHECK(all.starts_with(first));
    auto last = all.substr(first.size());
    CheckUtf8(last);
    CHECK(last.size() <= 64 * 1024 && last.size() > 63 * 1024);
    CHECK(last.ends_with(" [truncated]\r\n"));
}
void CheckRotation() {
    Case(L"rotation");
    Log(L"boundary");
    const auto active = LogDir() / L"application.log";
    const auto older = LogDir() / L"application.log.1";
    const auto oldest = LogDir() / L"application.log.2";
    auto record = Read(active);
    Write(active, std::string(LogLimit - record.size(), 'a'));
    Write(older, "previous\r\n");
    Write(oldest, "oldest\r\n");
    Log(L"boundary");
    CHECK(std::filesystem::file_size(active) == LogLimit);
    CHECK(Read(older) == "previous\r\n");
    auto full = Read(active);
    Log(L"next");
    CHECK(Read(older) == full && Read(oldest) == "previous\r\n");
    CHECK(Read(active).ends_with("next\r\n"));
    Write(active, std::string(LogLimit, 'b'));
    Log(L"newest");
    CHECK(Read(oldest) == full && Read(older) == std::string(LogLimit, 'b'));
    CHECK(!std::filesystem::exists(LogDir() / L"application.log.3"));
    // 旧版遗留的超限日志也纳入上限，不能只约束新文件。
    for (const auto &path : {active, older, oldest})
        Write(path, std::string(LogLimit + 1, 'x'));
    Log(L"after legacy oversized logs");
    CHECK(Read(active).ends_with("after legacy oversized logs\r\n"));
    CHECK(!std::filesystem::exists(older) && !std::filesystem::exists(oldest));
}
void CheckBusyAndUnavailable() {
    Case(L"busy");
    Log(L"before");
    const auto active = LogDir() / L"application.log";
    auto before = Read(active);
    {
        Handle locked{CreateFileW((LogDir() / L"application.log.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr)};
        CHECK(locked.value != INVALID_HANDLE_VALUE);
        OVERLAPPED range{};
        CHECK(LockFileEx(locked.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range));
        Log(L"must skip busy lock");
        CHECK(Read(active) == before);
    }
    // 已打开的读者允许追加，但拒绝重命名；轮转失败不能继续突破容量或提前挤掉备份。
    Write(active, std::string(LogLimit, 'r'));
    Write(LogDir() / L"application.log.1", "keep older");
    Write(LogDir() / L"application.log.2", "keep oldest");
    {
        Handle reader{CreateFileW(active.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        CHECK(reader.value != INVALID_HANDLE_VALUE);
        Log(L"must skip blocked rotation");
        CHECK(std::filesystem::file_size(active) == LogLimit);
        CHECK(Read(LogDir() / L"application.log.1") == "keep older");
        CHECK(Read(LogDir() / L"application.log.2") == "keep oldest");
    }
    Log(L"recovered");
    CHECK(Read(active).ends_with("recovered\r\n"));
    Case(L"unavailable");
    std::filesystem::create_directories(LogDir().parent_path());
    Write(LogDir(), "directory blocked by file");
    Log(L"must not throw");
    CHECK(Read(LogDir()) == "directory blocked by file");
}
std::wstring ConcurrentMessage(wchar_t writer, int item) {
    return L"writer=" + std::wstring(1, writer) + L" item=" + std::to_wstring(item) + L" " +
           std::wstring(30000, writer) + L" END=" + std::wstring(1, writer);
}
void CheckCompleteRecords(const std::string &bytes) {
    CheckUtf8(bytes);
    size_t start = 0;
    while (start < bytes.size()) {
        auto end = bytes.find("\r\n", start);
        CHECK(end != std::string::npos);
        auto prefix = bytes.find("] writer=", start);
        CHECK(prefix != std::string::npos && prefix < end);
        char writer = bytes[prefix + 9];
        CHECK(writer == 'A' || writer == 'B' || writer == 'P');
        auto body = bytes.find(' ', prefix + 16);
        CHECK(body != std::string::npos && body < end);
        ++body;
        CHECK(end - body == 30006);
        CHECK(bytes.compare(body, 30000, std::string(30000, writer)) == 0);
        CHECK(bytes.substr(body + 30000, 6) == " END=" + std::string(1, writer));
        start = end + 2;
    }
}
void CheckProcesses() {
    Case(L"processes");
    // 预先跨过两个轮转边界，检查并发追加后的所有保留文件仍只有完整记录。
    for (int i = 0; i < 300; ++i)
        Log(ConcurrentMessage(L'P', i));
    auto eventName = L"Local\\ZenExplorerLogTest-" + std::to_wstring(GetCurrentProcessId());
    Handle ready{CreateEventW(nullptr, TRUE, FALSE, eventName.c_str())};
    CHECK(ready.value);
    Handle children[2];
    for (int i = 0; i < 2; ++i) {
        auto command = Quote((ExeDir() / L"zen_logging_tests.exe").wstring()) + L" --writer " +
                       Quote(eventName) + L" " + std::wstring(1, wchar_t(L'A' + i));
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                             nullptr, &startup, &process));
        CloseHandle(process.hThread);
        children[i].value = process.hProcess;
    }
    CHECK(SetEvent(ready.value));
    for (const auto &child : children) {
        CHECK(WaitForSingleObject(child.value, 15000) == WAIT_OBJECT_0);
        DWORD code = 1;
        CHECK(GetExitCodeProcess(child.value, &code) && code == 0);
    }
    bool workerWrote = false;
    for (const wchar_t *name : {L"application.log", L"application.log.1", L"application.log.2"}) {
        auto bytes = Read(LogDir() / name);
        CHECK(bytes.size() <= LogLimit);
        CheckCompleteRecords(bytes);
        workerWrote |=
            bytes.find("] writer=A") != std::string::npos || bytes.find("] writer=B") != std::string::npos;
    }
    CHECK(workerWrote);
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 4 && std::wstring(argv[1]) == L"--writer") {
            Handle ready{OpenEventW(SYNCHRONIZE, FALSE, argv[2])};
            CHECK(ready.value && WaitForSingleObject(ready.value, 10000) == WAIT_OBJECT_0);
            for (int i = 0; i < 300; ++i) {
                Log(ConcurrentMessage(argv[3][0], i));
                SwitchToThread();
            }
            return 0;
        }
        testRoot =
            std::filesystem::current_path() / L".test-data" /
            (L"logging-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        CheckDisabledAndEncoding();
        CheckRotation();
        CheckBusyAndUnavailable();
        CheckProcesses();
        std::filesystem::remove_all(testRoot);
        std::cout << "logging passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
