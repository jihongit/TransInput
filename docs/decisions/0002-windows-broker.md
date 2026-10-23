# ADR-0002：Windows 独立后台与异步候选协议

日期：2026-10-21。状态：已实现并做隔离验证，未完成实际桌面应用验收。

## 决策

共享算法与词表继续由 Rust 实现。Windows 后台可执行文件使用小型 C++20 / 官方 SDK 宿主加载 Rust C ABI，承担命名管道、ACL、进程和 I/O 生命周期；不在宿主中重写拼音切分、词库查询或候选排序。这样复用已经验证的跨语言边界，不为少量平台传输接口再手写一套 Rust Win32 ABI。它是 Windows 承载方式的局部选择，不改变跨平台 Rust 核心与 Apple / Android 原生接入的决策。

TSF DLL 只做本地组合编辑、候选展示和系统上屏。词库只在独立后台加载；宿主软件的按键回调不读取词库、不调用管道等待、不等待后台线程结束。

## 协议与权限

- 每个登录会话独立的本机命名管道，DACL 仅授权当前 logon SID，拒绝远程客户端。客户端还核对服务端可执行文件路径；测试进一步固定自建子进程 PID。
- 帧包含固定 magic、版本、类型、长度、会话、文本修订号与请求 ID，按小端逐字段编码，无裸指针、STL、COM 对象或内存布局直接传输。负载上限 8 KiB，拼音最多 128 ASCII 字节。
- 后台查询只返回候选，不修改任何应用文本。客户端核对完整身份和返回原拼音，防止共享响应串用。
- 所有管道 I/O 使用 overlapped，超时或关闭后取消，并确认完成后才释放 OVERLAPPED / 缓冲区；等待只发生在工作线程。
- 后台使用固定 8 个处理线程，每个连接处理一个请求后释放；无请求 15 秒退出。响应后使用有时限的确认消息，不用可能无限等待读者的 FlushFileBuffers。
- x86 / x64 TSF 通过相同 wire 连接 `build/shared/` 后台。两种架构不在宿主内加载 Rust DLL，也不通过宽度不同的指针互操作。

## 输入顺序与失败

平台始终按顺序处理所有按键，后台收到的是完整拼音快照。仅未处理的候选预览允许替换为最新快照；已接受提交进入单独 FIFO，不能合并丢弃。

候选尚未返回就按空格 / 1 时，TSF 队列加入带唯一 ID 的待解析提交占位。占位之前的组合可更新，占位及之后的提交按原顺序等待解析；后续组合不能越过它写入。返回结果只填充原上下文中的对应占位，不重查“当前焦点”。过期预览按会话 / 文本修订号丢弃。

后台失败或输入法停用时，已接受但尚未解析的提交保留原拼音，避免无限等待或删除已消费文字。此为核心故障的降级，不是翻译失败时的行为；翻译故障应保持正常中文。最终提示与切换体验仍需真实宿主验证。

## 线程与卸载

后台线程不持有 COM / UI 对象。消息窗口把纯数据交回原线程，跨线程锁只覆盖短队列操作，不持锁做 I/O 或 COM 回调。每通道最多 32 个等待中的提交，包含在途 / 结果的总任务最多 64；每个模块最多 8 个通道，满载明确拒绝未接受工作。

停用只发停止信号、拆除通知窗口，不在 UI 线程 join。工作回调持有真实模块引用，由 `FreeLibraryWhenCallbackReturns` 在回调返回后释放，防止 DLL 已卸载而线程仍执行其中代码。回调可触发停用，因此窗口过程复制一次回调、调用后不再解引用窗口对象。

## 官方依据与范围

- [Named Pipe Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)：使用 logon SID 限制登录会话，不能依赖默认 DACL。
- [Canceling Pending I/O Operations](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)：取消并不等于已经完成，完成前不能复用 OVERLAPPED。
- [FreeLibraryWhenCallbackReturns](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-freelibrarywhencallbackreturns)：由线程池在回调完成后释放 DLL。

本机协议验证、消息窗口测试和 SDK 文本宿主测试不等于真实软件可见打字测试。公司电脑无管理员权限的边界保持不变，尚未修改系统注册或生成安装包。
