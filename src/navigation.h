#pragma once
#include "location.h"
namespace zen {
struct NavigationRequest {
    LocationKey destination;
    bool appendHistory = true;
    int historyIndex = -1;
};
// 新地址成功后提交；已有历史即使加载失败也提交其位置。
class NavigationState {
    NavigationRequest request;
    LocationResolution identity;
    bool pending = false;
    HRESULT error = S_OK;

  public:
    bool Pending() const {
        return pending;
    }
    HRESULT Error() const {
        return error;
    }
    const NavigationRequest &Request() const {
        return request;
    }
    void Begin(NavigationRequest next) {
        request = std::move(next);
        identity = {};
        pending = true;
        error = S_OK;
    }
    void SetIdentity(LocationResolution resolved) {
        identity = std::move(resolved);
    }
    bool Matches(PCIDLIST_ABSOLUTE pidl) const {
        return pidl && (identity.pidl.empty() ||
                        ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(identity.pidl.data()), pidl));
    }
    bool AcceptsCompletion(PCIDLIST_ABSOLUTE pidl) const {
        return pending && Matches(pidl);
    }
    // 返回值只表示本次失败是否允许尝试解析名称；取消绝不触发回退。
    bool Fail(HRESULT result) {
        bool fallback = pending && identity.savedIdentity && !Cancelled(result);
        identity = {};
        pending = false;
        error = result;
        return fallback;
    }
    void Complete() {
        request = {};
        identity = {};
        pending = false;
        error = S_OK;
    }
    static bool Cancelled(HRESULT result) {
        return result == E_ABORT || result == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
};
} // namespace zen
