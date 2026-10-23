# Windows 原型构建

当前验证环境：Windows 10 Enterprise x64 19045、VS 2022 Build Tools、MSVC 14.44.35207、Windows SDK 10.0.26100.0、Rust 1.99.1 / rustup 1.29.1。Rust 最低版本字段不是已经验证的兼容承诺；正式构建锁定本页版本。

需要 Visual Studio C++ 桌面生成工具和 Windows SDK。脚本通过 `vswhere` 查找，不要求全局配置开发者终端。

在本目录打开 PowerShell，首次准备项目本地 Rust：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\bootstrap-rust.ps1
```

下载到 `.tools/`，校验已固定的官方 rustup 安装器 SHA256，安装 Rust 1.99.1、rustfmt、clippy 及 x86 标准库，不修改系统 PATH。官方服务器不畅时可显式使用 `-Mirror rsproxy`；脚本先把镜像版本清单与官方清单哈希比较，rustup 再验证组件哈希。下载或校验失败即停止。

本机首次准备时已核对官方 stable 清单和镜像清单一致，安装的 stable 实际为 1.99.1；构建脚本同时检查编译器版本。`bootstrap-rust.ps1` 是后续重建入口，本轮未另起第二套全新目录完整重装，不能把它标为已完成干净机器安装验收。

执行全部 32 / 64 位检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-windows.ps1
```

包含词表源文件哈希 / 转换一致性、格式、Clippy、Rust 测试、release 构建、C ABI、中文与翻译后台协议 / 故障、WinHTTP 环回测试、DPAPI 虚构配置、COM 生命周期、真实 TSF 上下文测试。翻译测试只访问本机环回服务器，不读取真实 Key、不请求 DeepSeek。每个子进程有超时，仅终止本次测试子进程树。日志、检查结果及源码哈希在 `artifacts/p1/`。TSF 测试只在测试进程注册与激活临时 profile，不修改系统默认输入法，不进行桌面按键注入。固定 SDK 用例通过测试 DLL 的显式修饰键入口调用原有 sink，不读取或更改用户实时修饰键；正式 DLL 不提供这个入口，人工窗口仍使用真实按键捕获。

本地学习检查包括 C ABI、512 条边界、临时 DPAPI 文件、并发、损坏保护、磁盘失败和管理请求去重；TSF 测试使用独立父进程约束的候选后台和临时密文，不连接真实用户学习文件。正式 DLL 与测试 DLL 分别检查接口边界与卸载 / 重载身份。默认检查有独立 `--digits` / `--pages` 场景、`candidate-preview.exe` 六张 DPI 图片、设置两页图片和原生编辑器自检；学习管理的 37 项检查点见 [报告](testing/2026-10-22-learning-controls.md)，后续结果以新增检查报告为准。

混输定向运行方式为 `build\x64\tsf-integration.exe <TransInputPrototypeTest.dll绝对路径> --mixed`，32位替换为 x86。场景包括候选与写锁延迟、Caps 大小写、数字 / 符号、原上下文、取消 / 停用、学习与翻译隔离；使用固定 US ASCII 修饰键 fixture，不验证当前物理键盘布局。默认全套及 `native-input` 均已纳入该场景，详情见 [混输报告](testing/2026-10-22-mixed-input.md)。

词条内全简拼混写定向参数为同一测试程序的 `--spelling`；基础/个人检索、编辑、延迟候选、快速提交、学习和模拟翻译由此与 `--words` 覆盖。默认全套及 `native-input` 均包含新TSF场景。`target\release\transinput-cli.exe --benchmark-spelling`（x86改target子目录）测固定混写算法事件延迟；已纳入默认全套，不能代替实际宿主/UI延迟。修改Rust、词库或后台后必须默认全套，不能只运行native-input。详见 [混写报告](testing/2026-10-22-mixed-spelling.md)。

仅修改 TSF / 候选动作时，可在已有完整构建上执行 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-windows.ps1 -Architecture all -Scope native-input`。该模式只重建 TSF 与相关测试，检查词表、正式 / 测试 COM 边界及真实上下文；不重建 Rust 或后台，不重复网络测试。改动共享协议、Rust、后台或凭据实现时应使用默认全套检查。结果的 `scope` 用于区别全套和定向验证，不能将定向成功写成全套通过。

单独构建或演示 Rust 候选：

```powershell
cmd /c scripts\msvc.cmd x64 cargo build
cmd /c scripts\msvc.cmd x64 native
.\target\release\transinput-cli.exe nihaoshijie
```

32 位将 `x64` 改为 `x86`，Rust 输出在 `target/i686-pc-windows-msvc/release/`。CLI 默认使用已转换的第三方基础词表，仅为核心演示，不是系统输入法安装程序。`--benchmark` 输出本地算法计时，不包含候选窗绘制、真实按键投递或网络。

上游词表原件已在仓库中固定，无需构建时联网下载。运行 `powershell -NoProfile -File scripts\prepare-dictionary.ps1` 可重建 TSV，追加 `-VerifyOnly` 仅检查来源哈希、格式、数据行数与生成文件一致性，不修改文件。Git 属性保留原始与生成数据字节，避免 Windows 换行转换破坏哈希。

输出包括 `transinput_core.dll`、CLI、`build/<arch>/TransInputPrototype.dll` 和测试程序。另有只读部署检查和受签名/路径约束的底层注册助手，但没有经过验收的安装器或可日常替换旧版的发行包；不要用 `regsvr32` 安装此原型。原型只导出类工厂与卸载检查接口，独立系统 GUID 与旧版不同。

当前账户安装、签名与回滚的待办见 [Windows 安装准备](INSTALLATION.md)。

Windows 后台输出为 `TransInputBroker.exe` 与独立 `TransInputTranslator.exe`；设置为 `TransInputSettings.exe`（`--tray` 驻留托盘）。x64 构建把它们、原图标和 Rust DLL 放入 `build/shared/`，供两种架构连接；TSF 本身不加载 Rust DLL。TSF 已连接中文与翻译后台。后台按当前账户、隐藏运行，无请求 15 秒退出，不注册服务或开始菜单项。若上一轮测试仍占用构建文件，复制脚本最多等待 20 秒，不杀其他进程。

独立 `TransInputPrototypeTest.dll` 只连接无网络能力的翻译 fixture；正常 `TransInputPrototype.dll` 没有 fixture 覆盖入口。不要把测试 DLL、fixture 或测试程序纳入正式分发。设置的 `--render-preview <PNG 路径>` 只渲染自有隐藏窗口，使用虚构 Key，不读写实际设置、不加托盘、不联网；它不是桌面点击验收。

`build/<arch>/TransInputTestHost.exe` 是独立的原生 RichEdit 测试窗口。无参数打开交互窗口，`--self-check` 使用自身离屏、不激活的工具窗口执行固定中文、编辑、取消、翻页、选区、只读、焦点与快速提交检查；候选弹出被测试 DLL 抑制，不注入系统按键，不改变用户前台。测试窗口显式路由到自研服务，通道未就绪时阻止落到其他输入法而产生假通过；它验证原生文本存储和 TSF 编辑，不验证系统键盘接入已经安装。人工内容关闭不保存，不读取真实 Key，不联网、不学习、不改系统默认输入法；有配套 DLL、候选后台、核心 DLL 和无网络翻译 fixture 依赖，不能单独复制一个 EXE 使用。手动测试请使用无敏感信息的文字。

建议使用 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\open-input-test.ps1` 创建和自检隔离副本再打开；操作说明见 [INPUT_PAD.md](testing/INPUT_PAD.md)。正常启动必须先显示编辑框再等待 TSF 上下文，不能以隐藏自检替代启动路径。原生编辑器及定位见 [原生编辑器报告](testing/2026-10-22-native-editor.md)，本机Caps混输反馈见 [混输报告](testing/2026-10-22-mixed-input.md)，最新57项完整回归（包含基础与个人词的全简拼混写/排序）见 [混写报告](testing/2026-10-22-mixed-spelling.md)。

已有完整构建、仅修改测试编辑器时，`cmd /c scripts\msvc.cmd x64 native editor` 可只重建该 EXE；这不重建其余 DLL / 后台，也不是完整回归。GitHub 控制台工作流的准备与本机验证范围见 [CI 工具记录](../sources/CI.md)，远端尚未执行。

仅修改TSF集成测试可用 `native integration` 单独重建其EXE；`native words` 重建后台、设置、词库测试及共享副本，不执行完整回归。两者均需先有匹配Rust release构建。词库定向检查为 `words-smoke.exe <核心DLL绝对路径>`、`tsf-integration.exe <测试TIP绝对路径> --words`，设置专用 `TransInputSettingsTest.exe --self-check-words` 只操作自身隐藏控件与临时词库。正式设置EXE拒绝该自检参数；`--render-words-preview <PNG>` 仅用虚构内容预览。测试专用设置EXE、词库测试器和fixture均不纳入正式发行。

完整检查还包含每架构 10 秒固定文字资源场景和隔离副本自检。额外执行 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\soak-windows.ps1 -Architecture all -Seconds 60` 可分别采样两架构的编辑器及其自有后台；时长可设 10～28,800 秒。上限检查用于发现失控增长，不等于长期无泄漏证明。该测试不进行付费请求、不操作其他应用或个人文档。

默认全套检查没有付费请求。单独 `cmd /c scripts\msvc.cmd x64 live` 仅构建真实接口测试器；实际执行还要求 `--approved-fixed-cases` 和绝对核心 DLL 路径，会使用当前用户旧版已保存凭据发送固定九句文本，不能放进 CI。具体授权、执行和结果见 [真实接口记录](testing/2026-10-21-deepseek-live.md)。

当前没有引入 CMake、Qt、Flutter、WinUI 或第三方 Cargo crate。后续引入时需同步锁定版本、构建方式和许可证，避免文档与实际依赖不一致。

## 兼容接口与交付检查

默认完整检查增加候选UI协商、UI Automation组件和TSF `--uiless` 场景；后者是普通宿主中sink接管显示的测试，不代表严格UIELEMENTENABLEDONLY激活或真实游戏/读屏验收。`TransInputRegister.exe --self-check` 只检验内存事务，`--plan` 只输出计划；不在回归脚本执行系统注册。

`powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-delivery.ps1` 在artifacts内用固定虚构夹具检查导出白名单、已知凭据拒绝、私有文件拒绝、打包以及源码/二进制/双架构门槛。伪EXE/DLL只作为不可执行的文本夹具，绝不启动。默认完整检查包含这些脚本检查。

完整回归后，`scripts\package-windows.ps1 -Channel preview` 生成Windows开发预览ZIP和SHA256，校验同一轮x64/x86、源码与二进制哈希。它不会安装；`release`仍受签名和实际宿主验收门槛限制。`scripts\export-source.ps1` 只导出独立项目白名单，排除父工程、工具链、构建和个人数据，并拒绝目录链接及已知密钥模式。导出不是所有潜在秘密都不存在的保证，发布前仍审查清单。

`scripts\publish-github.ps1 -SourceDirectory <最新独立导出路径>` 仅用于初次建立空远端，已于2026-10-23执行完成。仓库已有历史后会主动拒绝再次初始化；后续请克隆并通过正常Git提交/分支维护，不使用强制推送。该脚本不执行正式发行。全部PowerShell命令的ExecutionPolicy参数仅影响调用进程，不修改系统策略。
