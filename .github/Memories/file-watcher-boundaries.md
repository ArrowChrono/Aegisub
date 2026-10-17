# 文件监听的平台与 GUI 边界

## 依赖边界

- `FileSystemWatcherBackend`、事件契约及 `WatchedFile` 的目标过滤／去抖核心保持与 wx 无关。平台 I/O 和 GUI 投递放在公共 core 之外。
- `src/watched_file_win.cpp` 当前是 Windows **GUI 宿主后端**，不是完全不依赖 wx 的 Win32 库。它使用原生目录 I/O，但仍依赖 `wxEvtHandler`、`wxThreadEvent` 和 wx 应用的待处理事件管理，将通知投递到 GUI 线程。“使用原生 I/O”不等于“完全 wx 解耦”。
- 现有 `CreateWxFileSystemWatcherBackend()` 工厂在 Windows 分派到该后端，非 Windows 保留 wx 实现。修改会影响现有主／副字幕监听及字体清单，不只影响新增窗体；不能把公共单元测试通过等同于所有调用者的 native E2E 已验证。

## 所有权与事件契约

- 后端独占目录／事件句柄、overlapped 操作及缓冲区、工作线程和 GUI 事件接收端。listener 为借用，仅在 GUI 线程访问；排队通知拥有自身路径和错误文本，不借用原生缓冲区。
- Reset 先撤销 listener，再发停止信号、取消并 drain 未完成 I/O、join 生产者，随后从 wx pending-handler 列表移除接收端并清理排队事件，最后释放句柄和存储。首次读请求已提交但线程构造失败时，回退路径同样必须 drain。
- 快速删除／同名重建时，检查旧句柄的 `DeletePending`。当前路径重新存在，不能证明旧句柄仍对应有效目录。确认删除则发目录 `Deleted`；后台监听期间的 I/O 错误通过调用者错误通道保持可见，不经全局日志弹出模态框；初始化失败仍走既有日志通道。
- 健康目录通知溢出时发目录 `Invalidated` 并继续监听。单文件调用者在自身监听目录失效时重读原目标；无关目录失效和普通目录修改不能变成该目标文件改动。
- 同一原生通知块内相邻 rename 配对保留旧、新路径；跨块或未配对通知允许表示为删除和创建，但已有调用者仍须感知两端受影响路径。

源码入口：[公共接口](../../src/watched_file.h)、[公共目标处理](../../src/watched_file_core.cpp)、[平台分派](../../src/watched_file_wx.cpp)、[Windows GUI 后端](../../src/watched_file_win.cpp)。
