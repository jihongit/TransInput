# Windows 本机验收版安装与维护

更新：2026-10-23。用户明确批准未签名本机验收安装；当前已部署至 `C:\Program Files\TransInput\0.1.0-local-preview`，x64/x86系统COM/TSF激活、停用/启用、实际卸载器卸载与重装检查通过。微软拼音、DeepSeek密文配置和加密备份保留；未设默认、未创建开始菜单入口、未更改Windows防护/证书信任/公司策略。详见 [实际安装报告](testing/2026-10-23-local-installation.md)。

## 使用

系统安装版首次真实输入反馈出现候选闪烁/卡顿，该项未通过。当前修复和验证见 [候选闪烁报告](testing/2026-10-23-candidate-flicker.md)；不能把系统注册通过当作真实应用输入已通过。

按Win＋空格选择TransInput；设置位于系统托盘（也可能在隐藏图标区）。现有DeepSeek配置继续使用，无须把Key发给维护者。“已安装的应用”中可找到TransInput本机验收版的卸载项。它仍是待实际应用验收的未签名测试版，不等于正式发行。

## 维护命令

使用已有PowerShell 7，不需要另一个管理员账号；当前账户权限已由用户实测，不能将这一结果推广为所有电脑免管理员安装。安装器不请求RunAs或修改安全策略。

```powershell
# 只读核对
& .\scripts\install-local-preview.ps1 -Action Verify
# 停用或重新启用，不删除个人资料
& .\scripts\install-local-preview.ps1 -Action Disable -AcceptUnsignedLocalPreview
& .\scripts\install-local-preview.ps1 -Action Enable -AcceptUnsignedLocalPreview
# 完整卸载本机验收版，保留个人配置和加密备份
& .\scripts\install-local-preview.ps1 -Action Uninstall -AcceptUnsignedLocalPreview
```

安装目录内的 `maintenance\install-local-preview.ps1` 同样支持上述维护操作，已测试从该脚本执行完整卸载，包括移除自身文件。文件占用时不强制关闭其他应用，保留卸载记录以便重试。

仅替换经过完整检查的候选界面修复 DLL，可使用源码中的 `update-local-preview-tip.ps1 -PackagePath <verified-preview>` 先查看计划，再加 `-Action Apply -AcceptUnsignedLocalPreview` 执行。旧 DLL 重命名保留供已打开进程继续使用，不直接覆盖映射字节；新进程加载新版，已有应用需要自行重新打开。此方式不替换共享后台或设置程序，具体检查点和备份写入安装记录。

## 从独立源码准备本机验收安装

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\verify-windows.ps1 -Architecture all -Scope all
$package = & .\scripts\package-windows.ps1 -Channel preview | ConvertFrom-Json
& .\scripts\install-local-preview.ps1 -Action Install -PackagePath $package.path -AcceptUnsignedLocalPreview
```

构建检查命令的ExecutionPolicy仅限该检查进程，不修改持久或公司策略；公司强制策略优先。实际安装/卸载使用已有PowerShell 7，不设置ExecutionPolicy。拒绝覆盖未知安装、注册或启动项；不能直接覆盖正在使用的版本目录升级。

## 保护条件与限制

独立local-preview入口必须显式确认、具有TransInput产品资源并位于固定受保护版本目录；原signed入口与release包继续要求可信签名。安装前后检查文件清单、哈希和重解析点，使用官方RegisterProfile/类别/InstallLayoutOrTip接口，失败仅回滚本项目。刚注册后的只读GetProfile核对最多追加两次新进程重试，注册写入和付费调用不重试。

当前已验证系统注册/加载和本项目自身进程激活，尚不等同于记事本、Codex、浏览器、Office、读屏、多屏和重启的完整验收。桌面自动化仍受1385限制；未修改系统策略来绕过。
