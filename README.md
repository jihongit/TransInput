<div align="center">
  <img src="assets/branding/original/transinput-icon.png" width="96" alt="TransInput 图标">
  <h1>TransInput</h1>
  <p>中文输入，自然美式英语。</p>
</div>

TransInput 是面向中文用户的 Windows 拼音输入法。本地完成中文输入，可选通过 DeepSeek 将当前选中的中文候选翻译为自然、流畅的美式英语，并直接提交中文、英文或中英双语。

输入引擎与交互逻辑自主实现，使用微软官方 Text Services Framework（TSF）接入 Windows。共享核心采用 Rust，Windows 接入与原生界面采用 C++20；复用授权明确的词库数据，不依赖现成输入法引擎。

> **开发预览版**：尚无正式签名发行包，仍存在首段输入不触发翻译及部分应用闪烁等[已知问题](docs/KNOWN_ISSUES.md)。请先在非重要文档中验证所用应用的兼容性。

## 功能

- **本地中文输入**：支持全拼、简拼、全简拼混写、候选选择、翻页和组合编辑。
- **可选 DeepSeek 翻译**：仅翻译当前选中中文；面向口语、商务和技术表达，默认采用自然美式英语，每次请求独立。
- **快捷提交**：可选择中文、英文、中英双语或原始拼音，无需复制粘贴。
- **中英文切换**：单按并松开 Shift 切换，支持 Caps Lock 和基础字母、数字、符号混输。
- **个人词库与选词偏好**：支持词条管理、本地学习、关闭学习和清空偏好；使用当前 Windows 账户加密保存。
- **浅色候选界面**：中文候选逐行显示，仅在选中行附上英文；长译文可滚动查看。

## 平台与运行方式

当前实现面向 Windows，提供 x64 和 x86 输入法组件。macOS、Android、iOS / iPadOS 暂无可用实现。

中文输入不需要联网或 API Key。翻译需要自行配置 DeepSeek API Key；未配置或关闭翻译时，使用普通中文输入。项目不提供离线翻译。

## 从源码构建

需要 Windows、Visual Studio 2022 C++ 桌面生成工具及 Windows SDK。脚本准备项目本地的固定版本 Rust 工具链，不修改系统 PATH。安装和维护入口使用 PowerShell 7。

```powershell
git clone https://github.com/jihongit/TransInput.git
cd TransInput

# 准备项目本地 Rust 工具链
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\bootstrap-rust.ps1

# 构建并执行 x64 / x86 完整检查
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\verify-windows.ps1
```

工具链版本、单独构建与测试入口见[构建文档](docs/BUILD.md)。默认检查使用固定虚构文本和本机故障服务，不调用真实 DeepSeek 接口。

完整构建后，可先打开隔离输入测试窗口：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\open-input-test.ps1
```

该窗口仅在自身进程接入输入法，不安装系统输入法，也不进行真实翻译。系统安装、预览包准备、权限要求、更新与卸载见[Windows 安装说明](docs/INSTALLATION.md)。未签名本机预览需显式选择相应安装模式，不应视为正式发行包。

## 使用

安装后，按 **Win + 空格** 选择 TransInput。系统输入标识显示 **TI**，模式按钮显示“中 / 英”。设置通过系统托盘中的 TransInput 图标进入；如果图标被折叠，请展开隐藏图标区。

### 配置翻译

打开 **设置 → 翻译**，启用 DeepSeek 翻译，填写 API Key 并保存。可使用“测试连接”检查配置，该操作会发送固定测试文本，可能产生 API 费用。

输入拼音后，使用方向键或鼠标选择中文，英文显示在对应中文旁边。只翻译当前候选，不同时请求整页候选；译文尚未就绪时，英文或双语快捷键不会提交旧译文。

### 快捷键

| 按键 | 操作 |
| --- | --- |
| Shift 单按并松开 | 切换中文 / 英文输入 |
| ↑ / ↓ | 切换中文候选 |
| PgUp / PgDn | 候选翻页 |
| Esc | 取消当前组合 |
| Enter | 提交输入的原始拼音 / 字母 |
| 空格 | 提交当前中文候选 |

**启用翻译时：**

| 按键 | 操作 |
| --- | --- |
| 1 | 提交当前选中中文 |
| 2 | 提交当前选中中文对应的英文 |
| 3 | 提交“中文 英文” |
| Ctrl + Enter，或点击英文 | 提交英文 |

**关闭翻译时：**数字 1–9 按候选序号选择中文。翻译模式与普通中文模式的数字规则不同，请以候选窗提示为准。

### 个人词库与学习

- **设置 → 个人词库**：添加或删除词条及其拼音。
- **设置 → 中文输入**：控制选词学习、清空偏好；清空偏好不会删除手工添加的词条。

更新输入法后，请保存文档并完全退出、重新打开使用它的软件。仅切换输入法或关闭文档标签，可能仍会保留旧版模块。

## 隐私与网络

- 中文拼音处理、词库查询和候选排序在本机完成，没有遥测或自动上传诊断功能。
- 启用翻译后，当前高亮中文候选稳定至少 160ms 才具备发送资格；这不是每次按键都请求模型。
- 每次翻译只包含固定系统提示词与当前中文，不携带聊天历史或周边文档。当前候选若包含个人词条，该内容也可能随翻译请求发送。
- API Key 和本地个人数据通过当前 Windows 账户的 DPAPI 加密保存；不要把 Key、真实输入、学习文件或未检查的日志提交到仓库。
- DeepSeek 调用受其服务规则与计费约束；远端数据处理由服务提供方负责。

详见[隐私说明](PRIVACY.md)。翻译规则可查看[系统提示词](prompts/translation-system-v1.txt)。

## 文档

| 文档 | 内容 |
| --- | --- |
| [构建与测试](docs/BUILD.md) | 工具链、构建命令与测试入口 |
| [安装与维护](docs/INSTALLATION.md) | 安装条件、更新、停用与卸载 |
| [已知问题](docs/KNOWN_ISSUES.md) | 使用限制及可用的临时处理方式 |
| [产品与交互](docs/PRODUCT.md) | 输入、候选和翻译行为 |
| [架构](docs/ARCHITECTURE.md) | 共享核心、平台接入与进程边界 |
| [验收要求](docs/ACCEPTANCE.md) | 功能、性能、故障与兼容性验证要求 |
| [第三方来源](sources/README.md) | 词库、依赖、固定来源与许可 |

## 参与贡献

欢迎提交问题反馈、文档改进和代码贡献。参与前请阅读[贡献指南](CONTRIBUTING.md)，在反馈中提供版本、系统与应用版本，以及使用固定虚构文本的复现步骤。

不要在 Issue、Pull Request 或附件中包含 API Key、真实输入或个人数据。安全问题请按[安全报告说明](SECURITY.md)处理。

## 许可与致谢

- 项目新写代码使用 [AGPL-3.0-only](LICENSE)。
- 基础词库数据来自 **rime-pinyin-simp / AOSP**，采用 Apache-2.0；保留其独立许可和来源，没有引入 Rime 输入法引擎。
- 第三方材料分别保留自身许可，详见 [NOTICE](NOTICE) 和[来源清单](sources/README.md)。
- 项目图标由维护者提供、通过 GPT 生成；品牌素材的来源与使用范围[单独记录](assets/branding/README.md)，不自动套用代码许可。

由 [jihongit](https://github.com/jihongit) 维护。
