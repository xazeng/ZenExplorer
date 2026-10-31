#include "operations.h"
#include <fstream>
#include <iostream>
using namespace zen;
static void Require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
// 提交进程随即退出；观察进程不持有工作进程句柄，不向其提供窗口。
int wmain(int argc, wchar_t **argv) {
    OleInitialize(nullptr);
    try {
        if (argc == 5 && std::wstring(argv[1]) == L"--submit") {
            bool ok = LaunchTask(nullptr,
                                 {std::wstring(argv[2]) == L"copy" ? Operation::Copy : Operation::Move,
                                  argv[4],
                                  {argv[3]},
                                  false},
                                 ExeDir() / L"ZenExplorer.exe");
            OleUninitialize();
            return ok ? 0 : 1;
        }
        auto base = std::filesystem::current_path() / L".test-data" /
                    (L"operation-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
        auto input = base / L"input", copied = base / L"copied", moved = base / L"moved";
        for (auto p : {input, copied, moved})
            std::filesystem::create_directories(p);
        SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", base.c_str());
        // 模拟主窗口已占用单实例标识：工作模式必须绕过这个互斥量。
        uint64_t hash = 1469598103934665603ull;
        for (auto c : ConfigDir().wstring())
            hash = (hash ^ c) * 1099511628211ull;
        HANDLE mainMutex =
            CreateMutexW(nullptr, FALSE, (L"Local\\ZenExplorer-" + std::to_wstring(hash)).c_str());
        Require(mainMutex != nullptr, "main instance mutex");
        std::string block(1024 * 1024, 'z');
        auto source = input / L"测试文件.bin";
        {
            std::ofstream f(source, std::ios::binary);
            for (int i = 0; i < 32; ++i)
                f.write(block.data(), block.size());
        }
        wchar_t self[32768];
        GetModuleFileNameW(nullptr, self, 32768);
        auto submit = [&](const wchar_t *op, const std::filesystem::path &src,
                          const std::filesystem::path &dst) {
            auto taskDir = DataDir() / L"tasks";
            size_t before = 0;
            std::vector<std::filesystem::path> existingResults;
            if (std::filesystem::exists(taskDir))
                for (auto &e : std::filesystem::directory_iterator(taskDir))
                    if (e.path().extension() == L".result") {
                        ++before;
                        existingResults.push_back(e.path());
                    }
            std::wstring cmd =
                Quote(self) + L" --submit " + op + L" " + Quote(src.wstring()) + L" " + Quote(dst.wstring());
            STARTUPINFOW si{sizeof(si)};
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            Require(CreateProcessW(self, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi) != 0,
                    "submit process creation");
            CloseHandle(pi.hThread);
            Require(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0, "submit process timeout");
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            Require(code == 0, "submission failed");
            bool complete = false;
            for (int attempt = 0; attempt < 600 && !complete; ++attempt) {
                size_t count = 0;
                if (std::filesystem::exists(taskDir))
                    for (auto &e : std::filesystem::directory_iterator(taskDir))
                        if (e.path().extension() == L".result") {
                            ++count;
                            auto result = ReadTaskResult(e.path());
                            Require(SUCCEEDED(result.invoked), "system command invocation result");
                            if (std::find(existingResults.begin(), existingResults.end(), e.path()) ==
                                existingResults.end()) {
                                auto detail = ReadTaskItems(e.path());
                                Require(detail.destination == dst.wstring() && detail.created.size() == 1,
                                        "result detail destination/top-level count");
                                Require(std::filesystem::exists(detail.created[0]),
                                        "actual result item missing");
                                Require(detail.created[0] != src.wstring(),
                                        "result reported source instead of new item");
                            }
                        }
                complete = count > before;
                if (!complete)
                    Sleep(100);
            }
            Require(complete, "worker completion timeout");
        };
        submit(L"copy", source, copied);
        auto copy = copied / source.filename();
        Require(std::filesystem::exists(source) && std::filesystem::file_size(copy) == 32 * 1024 * 1024,
                "copy contents/size");
        // 原目录复制应生成新名称，连续粘贴不能覆盖已有文件。
        submit(L"copy", source, input);
        submit(L"copy", source, input);
        int copies = 0;
        for (auto &entry : std::filesystem::directory_iterator(input)) {
            Require(entry.path().extension() == L".bin", "duplicate extension");
            Require(std::filesystem::file_size(entry.path()) == 32 * 1024 * 1024, "duplicate size");
            ++copies;
        }
        Require(copies == 3, "same folder repeated copy");
        auto folder = base / L"folder-source";
        std::filesystem::create_directories(folder / L"child");
        std::ofstream(folder / L"child" / L"payload.txt") << "folder payload";
        submit(L"copy", folder, base);
        int folders = 0;
        for (auto &entry : std::filesystem::directory_iterator(base))
            if (std::filesystem::exists(entry.path() / L"child" / L"payload.txt"))
                ++folders;
        Require(folders == 2, "same folder directory copy");
        submit(L"move", copy, moved);
        auto target = moved / source.filename();
        Require(!std::filesystem::exists(copy) && std::filesystem::file_size(target) == 32 * 1024 * 1024,
                "move result");
        std::ifstream f(target, std::ios::binary);
        std::string actual(block.size(), 0);
        for (int i = 0; i < 32; ++i) {
            f.read(actual.data(), actual.size());
            Require(actual == block, "file data mismatch");
        }
        // 删除目标后，从相同源再次复制到同一目标，不能误判源目标相同。
        submit(L"copy", source, copied);
        Require(std::filesystem::remove(copied / source.filename()), "remove destination");
        submit(L"copy", source, copied);
        Require(std::filesystem::file_size(copied / source.filename()) == 32 * 1024 * 1024,
                "copy again after deletion");
        Require(!std::filesystem::exists(ConfigDir() / L"session.dat"), "worker must not write main session");
        CloseHandle(mainMutex);
        std::cout << "independent copy/move, submitter exit, task results and data integrity passed\n";
        OleUninitialize();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        OleUninitialize();
        return 1;
    }
}
