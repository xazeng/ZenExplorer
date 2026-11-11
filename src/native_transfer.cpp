#include "native_transfer.h"

namespace zen {
HRESULT NativePaste(IDataObject *source, IShellItem *destination, bool *submitted) {
    if (submitted)
        *submitted = false;
    if (!source || !destination)
        return E_INVALIDARG;
    LONG references = 0;
    ComPtr<IUnknown> thread;
    HRESULT hr = SHCreateThreadRef(&references, &thread);
    if (FAILED(hr))
        return hr;
    hr = SHSetThreadRef(thread.Get());
    if (FAILED(hr))
        return hr;
    PIDLIST_ABSOLUTE destinationId = nullptr;
    SHGetIDListFromObject(destination, &destinationId);
    bool dropped = false;
    {
        ComPtr<IDropTarget> target;
        hr = destination->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target));
        if (SUCCEEDED(hr)) {
            DWORD effect = DROPEFFECT_COPY;
            constexpr DWORD keys = MK_LBUTTON | MK_CONTROL;
            hr = target->DragEnter(source, keys, POINTL{}, &effect);
            if (SUCCEEDED(hr)) {
                if (!(effect & DROPEFFECT_COPY)) {
                    target->DragLeave();
                    hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                } else {
                    effect = DROPEFFECT_COPY;
                    dropped = true;
                    if (submitted)
                        *submitted = true;
                    hr = target->Drop(source, keys, POINTL{}, &effect);
                }
            }
        }
    }
    // Drop 可在返回后继续处理。即使调用失败，也先让系统完成已启动的清理。
    // 此等待只存在于工作进程，不能移到主窗口，也不能用固定延时替代。
    SHSetThreadRef(nullptr);
    thread.Reset();
    while (InterlockedCompareExchange(&references, 0, 0) > 0) {
        MsgWaitForMultipleObjectsEx(0, nullptr, 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    // 原生流接收取消时可能删除半成品却未通知所有已打开的视图。
    // 只通知目录失效，让 Shell 自行枚举；不在主窗口同步刷新或推断文件清单。
    if (dropped && destinationId)
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSH, destinationId, nullptr);
    CoTaskMemFree(destinationId);
    return hr;
}
} // namespace zen
