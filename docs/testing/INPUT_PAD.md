# TransInput 本机输入测试窗口

这是独立测试程序，只在自己的原生 RichEdit 编辑框接入自研 TransInput。无需管理员权限，不安装系统输入法，不改默认设置或旧版，不创建开始菜单入口。翻译与学习关闭；不会读取实际 Key。手动输入在关闭后不保存，请只用不含敏感信息的测试文字。

完成构建后，从新项目目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\open-input-test.ps1
```

脚本将五个必要程序 / DLL 复制到独立目录，记录 SHA256，并先运行副本自检。失败时不打开交互窗口。这样可保留试用版本，也避免后续构建覆盖正在运行的文件。`-Architecture x86` 可试 32 位版本；`-NoOpen` 仅准备并自检副本。

关闭 Caps Lock 后，在窗口内测试：

1. `nihao` + 空格，应提交“你好”。
2. 连续快速输入几次 `nihao` + 空格，应依次上屏。
3. `n` + PgDn + 2，应选择第二页的第二个中文候选。
4. `ceshi` + Esc，应取消本次组合，不删除已上屏内容。
5. 在组合中使用左右、退格和 Delete，或选中已上屏文字后重新输入。
6. 切换到其他窗口再回来，继续输入；中文不应提交到其他窗口。

Enter 提交原拼音。基础混输使用键盘的“大写锁定（Caps Lock / CapsLk）”键，通常在 A 键左边、左 Shift 键上方；按一次开启，再按一次关闭。输入 `nihao` 暂不提交，开启大写锁定后输入 `GPT`，再关闭大写锁定输入 `ceshi` + 空格，应得到“你好GPT测试”。英文大小写遵循 Caps / Shift；英文状态下邮箱、路径和符号按原字符输入，不自动识别小写英文。也可试 `nihao` + 空格后立即输入 `2026`，应得到“你好2026”。Ctrl+A / Delete 由编辑框处理。候选可点选、翻页；滚动 / 多屏 / 缩放需按当前试用版本另行验收。

`TransInputTestHost.exe --self-check` 使用固定虚构文本和 SDK 回调验证原生文本存储，不注入物理键盘。自动夹具使用屏幕范围外、不激活的自身工具窗口，并抑制候选弹出；断言它不进入可见桌面、不成为前台，不要求用户暂停使用键盘。固定修饰键只作为测试 DLL 的调用参数，不更改系统键盘状态。`--soak-seconds=60` 在该离屏编辑框持续输入并采样自有三个进程的资源；允许 10～28,800 秒。也可运行 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\soak-windows.ps1 -Architecture all -Seconds 60`。

测试窗口显式将自己的键盘消息路由给自研服务，不能证明已完成系统安装、全局 TSF 键盘接入或记事本 / 浏览器 / Office 兼容。若未显示窗口，保留该副本目录中的 `manual.stderr.txt`；不要通过提高权限或修改公司策略解决。
