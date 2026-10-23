# Windows 首版交付状态

更新：2026-10-23。本轮用户要求直接完成剩余工作，按既定 Windows 首版持续实施，不再逐功能请求继续。Apple/移动端仍为后续独立平台阶段。

## 已实现并通过本机完整回归的范围

- 候选使用官方 ITfUIElementMgr 协商显示；宿主可接管当前页列表、选择/提交/取消。结束时断开旧接口并释放原文档；宿主保留引用会延长DLL寿命，但不能控制后续组合。
- 候选 UI Automation 提供 IME_Candidate_Window 标识、候选名称、片段导航/坐标、选中状态、选择/执行与显示/关闭事件；不夺取编辑框焦点，旧行/已关闭窗口拒绝执行。只测自身组件，不等同于 Narrator、Office 等实际应用通过。
- 显示/隐藏/移动发送系统 IME 事件，更新自身候选窗 owner；窗口销毁清除 HWND，无障碍模型重新创建时分配新身份，旧接口不会复活。
- 原生只读部署诊断 TransInputPreflight.exe：输出调用进程的提升状态、签名校验和机器COM键打开结果。不会写注册表、安装证书、请求提升或更改默认输入法。
- 新增受信任签名/产品标识/Program Files目录约束的底层注册助手；计划/内存回滚检查通过，开发构建执行写入口被提前拒绝。没有成功执行系统注册或完成安装器编排。
- 自写单色输入指示图标与产品版本资源已嵌入EXE/DLL；原始彩色图标保留不变。
- 预览包脚本：要求完整回归、源码哈希和二进制哈希匹配；签名不足不得输出正式发行包。包含许可/来源、文件清单、ZIP与SHA256，不夹带测试DLL、fixture、Key或个人数据。
- 源码独立白名单导出及已知凭据模式检查；只发布审核后的导出副本。GitHub脚本校验账号jihongit、拒绝覆盖已有远端历史、不使用force，不在文件/命令行输出Token。

## 实际证据

- UI接口/读屏接口组件和TSF宿主协商定向检查通过；个人词恢复检查通过，见 `artifacts/delivery-probe/20261023-000303-202/`。
- 导出/打包8项固定虚构夹具通过：`artifacts/delivery-tests/20261022-235725-565/`。包括learning.rs应保留、个人密文/固定伪密钥拒绝、路径范围、正式包签名门槛、测试后源码/二进制变化拒绝。
- 桌面工具按官方computer-use入口重试仍报CreateProcessWithLogonW 1385；未调用替代工具绕过。
- 本机无已发现的CurrentUser代码签名证书；开发DLL的WinVerifyTrust为0x800b0100（未签名）。工具执行进程报告elevated=true、机器COM键打开成功，但这不证明用户交互登录会话的部署权限。遵循用户明确的无管理员权限约束，不借此持久注册。
- GitHub已由用户在本机授权；认证API核对jihongit/id32673991，并确认同名仓库不存在后创建并推送 [jihongit/TransInput](https://github.com/jihongit/TransInput)。首次远端x64/x86 CI通过，私密漏洞报告已启用；详见 [源码公开记录](testing/2026-10-23-github.md)。不要求密码/Token发送到聊天或进入源码。

最新完整检查为`artifacts/p1/20261023-002335-421/`，**72/72通过**，两架构各46个Rust单元+6个词表集成，102个构建相关源码/数据、55个二进制/PNG快照。开发预览ZIP已生成并核对33个内容文件、清单和哈希；两架构各600秒持续输入分别6612/6616次中文提交正确，暖态增长上限未触发。最终独立源码导出/压缩包位于`artifacts/source-export/20261023-windows-preview/`。详细结果、失败及未执行项见 [交付测试报告](testing/2026-10-23-delivery.md)。

## 必须保持的发布门槛

1. 完整回归及固定构建产物哈希匹配。严格UILess标志激活在进程内profile环境曾失败（0x80070057）；普通宿主协商通过不能代替机器级兼容类别/严格激活验收。
2. 受信任代码签名。微软明确要求第三方输入法签名；当前不创建/信任自签根证书、不关闭防护。可评估SignPath Foundation，但其接受/身份/审批由外部服务决定，不能承诺新项目可立即获批。
3. 公司允许的签名部署渠道及实际宿主验收，包含安装、并存、升级/回滚、重启、卸载。当前无已验收的正式安装器，预览ZIP不是安装成功。
4. 当前版本长时资源报告、真实应用输入与显示/无障碍验收，不能用短时SDK结果替代。
5. 独立源码公开、首次远端双架构CI及私密安全报告入口已完成；后续每个发行提交仍须核对其对应CI，tag、签名安装包和发行说明需与实际版本一致。

## 依据

- [微软第三方输入法要求](https://learn.microsoft.com/en-us/windows/apps/develop/input/input-method-editors)
- [微软自定义输入法/安装/图标/无障碍要求](https://learn.microsoft.com/en-us/windows/apps/develop/input/input-method-editor-requirements)
- [UILess模式与宿主协商](https://learn.microsoft.com/en-us/windows/win32/tsf/uiless-mode-overview)
- [UIA提供程序线程模型](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcore/ne-uiautomationcore-provideroptions)
- [SignPath Foundation申请条件](https://signpath.org/terms)：不是已获签名或已提交申请的声明；不代用户同意外部条款或发送申请。
