#include "app.h"
#include "operations.h"
#include <gdiplus.h>
using namespace zen;
int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int show) {
    if (FAILED(OleInitialize(nullptr)))
        return 1;
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&cc);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        OleUninitialize();
        return 2;
    }
    // 在图形初始化、配置读取和单实例检查之前分流，任务不依赖主窗口。
    if (argc > 1 && std::wstring(argv[1]) == L"--task") {
        std::filesystem::path task = argc == 3 ? argv[2] : L"";
        LocalFree(argv);
        int result = task.empty() ? 2 : RunFileTask(h, task);
        OleUninitialize();
        return result;
    }
    std::wstring path = argc > 1 ? argv[1] : L"";
    LocalFree(argv);
    Gdiplus::GdiplusStartupInput graphicsInput;
    ULONG_PTR graphicsToken = 0;
    if (Gdiplus::GdiplusStartup(&graphicsToken, &graphicsInput, nullptr) != Gdiplus::Ok) {
        OleUninitialize();
        return 1;
    }
    int code = 0;
    {
        App app;
        code = app.Run(h, show, path);
    }
    Gdiplus::GdiplusShutdown(graphicsToken);
    OleUninitialize();
    return code;
}
