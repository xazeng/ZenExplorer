#include "shell_menu.h"
namespace zen {
ShellMenu::~ShellMenu() {
    if (menu)
        DestroyMenu(menu);
    if (attachedSite)
        attachedSite->SetSite(nullptr);
}
HRESULT ShellMenu::Create(IShellView *view, bool background, HWND window, UINT flags) {
    if (!view || !IsWindow(window) || menu || context)
        return E_INVALIDARG;
    owner = window;
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&folderView))) &&
        SUCCEEDED(folderView->GetFolder(IID_PPV_ARGS(&folder))) && SUCCEEDED(folder->GetCurFolder(&pidl))) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item))))
            directory = ItemName(item.Get(), SIGDN_FILESYSPATH);
    }
    CoTaskMemFree(pidl);
    // 特殊位置没有文件系统目录时保持空值，不把 Shell 解析名当成工作目录。
    if (!directory.empty()) {
        int count = WideCharToMultiByte(CP_ACP, 0, directory.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (count > 0) {
            directoryAnsi.resize(count);
            WideCharToMultiByte(CP_ACP, 0, directory.c_str(), -1, directoryAnsi.data(), count, nullptr,
                                nullptr);
        }
    }
    HRESULT hr = view->GetItemObject(background ? SVGIO_BACKGROUND : SVGIO_SELECTION, IID_PPV_ARGS(&context));
    if (FAILED(hr))
        return hr;
    ComPtr<IObjectWithSite> hosted;
    if (SUCCEEDED(context.As(&hosted))) {
        ComPtr<IUnknown> existing;
        // 保留系统已有站点；缺失时关联来源视图，让新建菜单调用原生选中与名称编辑。
        if (FAILED(hosted->GetSite(IID_PPV_ARGS(&existing))) || !existing) {
            hr = hosted->SetSite(view);
            if (FAILED(hr)) {
                Log(L"Shell 菜单关联视图失败 result=" + std::to_wstring((unsigned long)hr));
                return hr;
            }
            attachedSite = hosted;
        }
    }
    menu = CreatePopupMenu();
    if (!menu)
        return HRESULT_FROM_WIN32(GetLastError());
    context.As(&menu2);
    context.As(&menu3);
    if (!background)
        flags |= CMF_CANRENAME;
    return context->QueryContextMenu(menu, 0, 1, 0x6fff, flags);
}
std::wstring ShellMenu::Verb(UINT command) const {
    if (!context || command < 1 || command > 0x6fff)
        return {};
    wchar_t text[256]{};
    if (SUCCEEDED(context->GetCommandString(command - 1, GCS_VERBW, nullptr, (LPSTR)text, 256)))
        return text;
    char ansi[256]{};
    if (FAILED(context->GetCommandString(command - 1, GCS_VERBA, nullptr, ansi, 256)))
        return {};
    MultiByteToWideChar(CP_ACP, 0, ansi, -1, text, 256);
    return text;
}
UINT ShellMenu::Find(const std::wstring &verb) const {
    std::function<UINT(HMENU)> find = [&](HMENU m) -> UINT {
        for (int i = 0; i < GetMenuItemCount(m); ++i) {
            UINT id = GetMenuItemID(m, i);
            if (!_wcsicmp(Verb(id).c_str(), verb.c_str()) &&
                !(GetMenuState(m, i, MF_BYPOSITION) & (MF_DISABLED | MF_GRAYED)))
                return id;
            if (auto sub = GetSubMenu(m, i))
                if (UINT result = find(sub))
                    return result;
        }
        return 0;
    };
    return menu ? find(menu) : 0;
}
HRESULT ShellMenu::Invoke(UINT command, POINT point, std::optional<DWORD> modifiers) {
    if (!context || !menu || command < 1 || command > 0x6fff)
        return E_INVALIDARG;
    CMINVOKECOMMANDINFOEX info{sizeof(info)};
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    if (modifiers)
        info.fMask |= *modifiers & (CMIC_MASK_SHIFT_DOWN | CMIC_MASK_CONTROL_DOWN);
    else {
        if (GetKeyState(VK_SHIFT) & 0x8000)
            info.fMask |= CMIC_MASK_SHIFT_DOWN;
        if (GetKeyState(VK_CONTROL) & 0x8000)
            info.fMask |= CMIC_MASK_CONTROL_DOWN;
    }
    info.hwnd = owner;
    info.lpVerb = MAKEINTRESOURCEA(command - 1);
    info.lpVerbW = MAKEINTRESOURCEW(command - 1);
    info.lpDirectory = directoryAnsi.empty() ? nullptr : directoryAnsi.c_str();
    info.lpDirectoryW = directory.empty() ? nullptr : directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    info.ptInvoke = point;
    HRESULT hr = context->InvokeCommand((CMINVOKECOMMANDINFO *)&info);
    Log(L"Shell 菜单 command=" + std::to_wstring(command) + L" verb=" + Verb(command) + L" directory=" +
        directory + L" result=" + std::to_wstring((unsigned long)hr));
    return hr;
}
} // namespace zen
