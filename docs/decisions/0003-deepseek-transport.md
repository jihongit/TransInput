# ADR-0003：独立 DeepSeek 后台与 Windows 网络实现

日期：2026-10-21。状态：后台、TSF 翻译交互与设置原型已连接；隔离回归及九句已授权固定文本真实接口通过，真实宿主验收待完成。

## 决策

中文候选继续由 `TransInputBroker.exe` 提供；另建 `TransInputTranslator.exe` 承载翻译调度、凭据和 HTTPS。两者没有共享的网络锁或工作队列。TSF 通过有界异步通道交换纯数据，不加载联网代码。

共享调度和提示词由 Rust 核心提供；Windows 网络使用系统 WinHTTP 异步 API，JSON 使用 Windows.Data.Json 系统 API / C++/WinRT 投影，不手写 TLS、加密或 JSON 解析器。C++/WinRT MIT 许可保存在 `sources/cppwinrt/`。这一选择不引入其他输入法引擎，也不改变 Rust 共享核心方案。

## 请求与时限

- 固定地址 `https://api.deepseek.com/chat/completions`；不提供用户可配置的任意地址。环回地址只编译进测试程序。
- 核对当日官方文档后使用 `deepseek-flash`，显式 `thinking.type=disabled`、`temperature=0.2`、`stream=false`、`max_tokens=1024`。未来模型变更先核对文档并更新记录。
- 每个请求恰好两条消息：版本化 system 提示词和本次中文 user 原文，无 tools、历史消息或应用周边文本。复用一个 HTTPS 会话有助于连接复用，不代表多轮对话。
- 不自动重发投递结果不明的付费请求；失败后保持失败状态，显式重试才产生新任务。
- 传输总预算 3 秒；独立 actor 轮询执行超时 / 取消。响应 JSON 不超过 32 KiB，最终译文不超过 4 KiB；截断、异常结构、工具调用、额外思考内容、空白和危险控制字符拒收。
- 禁止自动重定向、Cookie、自动身份验证和自动登录；系统 TLS 证书校验保持开启。

## 关闭与资源

候选稳定 160 ms 后才调度，最多一个在途任务和一个最新待发文本。发出取消不释放任务名额，等 WinHTTP 最终 `HANDLE_CLOSING` 回调确认后才释放请求缓冲区并向 Rust 完成任务。父连接 / 会话关闭同样确认最终回调。

这些关闭等待只在独立翻译进程执行，不能进入 TSF 或中文候选后台。3 秒是请求取消预算，不是对操作系统最终回调完成时间的绝对保证；若系统关闭回调异常，翻译名额保持占用而不让新的请求重叠。正式版还需加入独立进程看护与故障提示，并做真实网络和长时验证。

## 配置与上下文

- 新版配置路径为当前用户 `%LOCALAPPDATA%\TransInputNext\settings.bin`，默认关闭。整个配置用当前用户 DPAPI 加密；保存时生成新配置身份，写入受当前用户 DACL 保护的临时文件，再原子替换。无管理员操作。
- 设置原型已实现，见 ADR-0004；离屏布局与后端配置分别验证，真实点击待验收。没有创建真实新版配置或迁移旧版设置。独立真实接口测试只在内存使用旧版已保存凭据，旧配置前后保持一致。
- 新后台管道仅当前 logon SID 可访问，拒绝远程连接；客户端核对可执行文件和测试 PID。服务端以客户端 PID、进程创建时间、本地组合编号共同分配内部会话，避免不同应用编号碰撞。
- 每个结果还匹配修订号和配置 epoch。只读查询不消费待发任务；查询他人上下文不返回译文；旧 revision 不覆盖新候选，旧焦点清理不清除新组合。最多保留 256 个上下文元数据条目，复用前淘汰非当前条目。
- 输入候选、译文和 Key 不写常规日志。默认回归只使用固定虚构文本和虚构 Key；单独授权的真实 API 测试输出仅包含固定虚构样例译文和耗时，不记录凭据。

## 官方依据

- [DeepSeek Chat Completions](https://api-docs.deepseek.com/api/create-chat-completion/)：当前模型、思考开关、消息和返回结构。
- [WinHTTP close](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle)：关闭后仍可能回调，不能提前释放上下文。
- [WinHTTP callback](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nc-winhttp-winhttp_status_callback)：最终关闭通知。
- [Windows.Data.Json](https://learn.microsoft.com/en-us/uwp/api/windows.data.json.jsonobject)：系统 JSON 读写。
- [CryptProtectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)：当前用户数据保护；没有使用 LOCAL_MACHINE 范围。

实际结果见翻译后台测试报告；隔离测试不能代替真实 DeepSeek 质量、可见候选交互和日常可用性验收。
