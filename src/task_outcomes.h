#pragma once
#include "platform.h"
#include <sherrors.h>
#include <unordered_map>
namespace zen {
struct TaskOutcomeSummary {
    HRESULT failure = S_OK;
    bool incomplete = false;
};
class TaskOutcomes {
    std::unordered_map<std::wstring, HRESULT> unresolved;

  public:
    // 同一来源以后一次通知为准；成功终态不再保留路径，也撤销此前失败或未决结果。
    void Record(const std::wstring &source, HRESULT hr) {
        if (FAILED(hr) || hr == COPYENGINE_S_USER_IGNORED || hr == COPYENGINE_S_PENDING ||
            hr == COPYENGINE_S_PENDING_DELETE || hr == COPYENGINE_S_PENDING_BATCH_COPY)
            unresolved[source] = hr;
        else
            unresolved.erase(source);
    }
    TaskOutcomeSummary Summarize(HRESULT operationFailure = S_OK) const {
        TaskOutcomeSummary result{operationFailure, !unresolved.empty()};
        for (const auto &[source, hr] : unresolved)
            if (FAILED(hr) && SUCCEEDED(result.failure))
                result.failure = hr;
        return result;
    }
    size_t RetainedCount() const {
        return unresolved.size();
    }
};
} // namespace zen
