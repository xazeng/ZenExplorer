#pragma once
#include "operations.h"

namespace zen {
// 在已初始化 OLE 的独立 STA 内复制数据对象。返回前等待 Shell 释放线程引用；
// 成功表示系统调用完成，不提供逐项结果，也不推断系统是否生成了撤销记录。
// submitted 为 true 时，操作提示由原生目标负责，调用者不再重复显示传输错误。
HRESULT NativePaste(IDataObject *source, IShellItem *destination, bool *submitted = nullptr);
} // namespace zen
