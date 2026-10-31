# ZenExplorer

面向 Windows 10 及以上 x64 的原生双栏文件管理器原型。C++/Win32 主窗口承载 Windows Shell 文件视图，文件任务通过独立进程调用系统操作 UI。当前不是已完成全部交互验收的正式版本。

## 运行

运行 `ZenExplorer.exe` 即可。发布目录只需要这一个 EXE；主窗口和独立文件操作复用同一个程序，通过内部 `--task` 参数区分模式。静态链接 MSVC 运行库，不需要额外工作进程 EXE 或本项目专用运行时。

首次运行左右各打开“此电脑”。可用地址栏输入路径，也可在命令行传入一个目录参数。再次启动会唤起已有窗口，传入的目录在活动栏末尾打开。

当前使用新的配置与历史拆分格式，不兼容或迁移旧会话及备份；没有新配置时按首次启动处理。旧文件任务不会自动重放。

程序数据默认位于 EXE 同级目录，不依赖启动时的工作目录：

- `config/session.dat`：标签、分组、当前目录、主目录、视图和窗口设置；`session.dat.bak` 为上一份会话备份。
- `data/history`：各标签导航历史及最近访问的快照，与配置及其备份按同代配对恢复。
- `data/tasks`：文件任务及完成结果。崩溃遗留任务不自动重放，以免重复操作。
- `log/application.log`：诊断日志，默认关闭；仅在启动前显式设置 `ZENEXPLORER_FILE_LOG=1` 时写入。关闭时不创建日志目录或文件。

测试时可设置环境变量 `ZENEXPLORER_ROOT_DIR` 指向独立根目录，其下仍分为 `config`、`data`、`log`；原 `ZENEXPLORER_DATA_DIR` 不再使用。不同配置目录使用不同单实例标识。不要手工修改正式配置或任务文件。程序目录需可写，保存失败会提示，不回退写入 AppData。更新程序时保留同级 `config`、`data`，移动程序时一并移动这两个目录。

## 构建与测试

要求 Visual Studio 2022 的 MSVC C++ 工具链、Windows SDK、CMake 3.24 及以上。当前使用 SDK 10.0.26100.0 编译；最低系统接口目标为 Windows 10。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix build/package
```

`build/package` 是发布目录，只包含运行所需的 `ZenExplorer.exe`，暂不生成 ZIP 发布包。测试程序保留在构建目录，不随发布包分发。

测试在工作目录的 `.test-data` 中生成会话和文件操作样本，不操作用户原有目录。文件操作测试每次产生 32 MB 源文件及相应输出，供检查；可在确认测试结束后清理。

## 当前边界

主界面和 Shell 视图共用一个 UI 线程。慢共享目录、Shell 扩展或同步属性调用可能阻塞整个窗口，不能承诺此时仍能关闭单个标签。

普通文件复制、移动和删除使用独立进程中的 IFileOperation，复用系统操作界面。已通过实际键盘粘贴、跨栏拖入子文件夹、资源管理器拖入，以及关闭主窗口后完成系统冲突处理的验证。暂不支持虚拟文件、右键拖放及创建快捷方式；不支持的来源不回退到主窗口传输。仅允许移动且无异步回执的来源会被拒绝。网络、权限异常、第三方扩展及外部接收方的完整行为仍需验收，详见实现记录。

## 文档导航

| 文档 | 内容 |
| --- | --- |
| [BUSINESS.md](BUSINESS.md) | 当前有效需求、交互规则、验收要求及待确认事项 |
| [TECHNICAL.md](TECHNICAL.md) | 当前技术决策与边界 |
| [IMPLEMENTATION.md](IMPLEMENTATION.md) | 当前实现、入口覆盖、已验证证据及待验证范围 |
| [历史记录](docs/history/IMPLEMENTATION-2026-10-31.md) | 历次过程快照，仅供追溯，不作为当前要求 |

需求已确认不代表已实现，已实现不代表已通过完整验收；验证状态统一查看 IMPLEMENTATION.md。
