# 语言与界面调研依据

归档日期：2026-10-21。资料来自此前方案讨论中的公开一手仓库和官方文档，作为架构依据；未对这些项目执行构建、性能测量或全面许可审计。正式采用组件时重新核对具体版本。

## 已核实的公开实践

| 来源 | 可核实事实 | 对 TransInput 的工程判断 |
| --- | --- | --- |
| [librime README](https://github.com/rime/librime) | 项目明确描述为跨平台 C++ 模块化引擎，列出不同系统前端 | 算法与数据层共享、系统接入分离是一条已有实践的路线 |
| [鼠须管 Swift 输入控制器](https://github.com/rime/squirrel/blob/master/sources/SquirrelInputController.swift) | Swift 控制器使用 InputMethodKit | 即使核心为 C++，Apple 接入层也可以使用 Swift |
| [Mozc 平台说明](https://github.com/google/mozc)与[转换模块](https://github.com/google/mozc/tree/master/src/converter) | 转换模块为 C++；项目提供多个桌面平台和 Android 原生库构建路径 | C++ 共享核心是成熟且可行的备选，不能因选择 Rust 就否定它 |
| [Fcitx5 Android 构建配置](https://github.com/fcitx5-android/fcitx5-android/blob/master/app/build.gradle.kts) | Kotlin Android 层整合原生引擎 / JNI 构建 | 平台生命周期和 UI 使用本地生态，算法可以放原生库 |
| [FlorisBoard 构建配置](https://github.com/florisboard/florisboard/blob/main/app/build.gradle.kts) | 使用 Kotlin、Compose 与 Android UI 依赖 | 现代声明式 UI 可以用于输入法；不据未合并提案声称主线引擎已经改用 Rust |
| [azooKey](https://github.com/azooKey/azooKey) | 项目说明使用 Swift、自研转换引擎与 SwiftUI；另有 macOS 版本 | 聚焦 Apple 平台时 Swift 可覆盖较多层，不能据此保证相同方案适合 Windows / Android |
| [Windows Chewing TIP 配置](https://github.com/chewing/windows-chewing-tsf/blob/main/tip/Cargo.toml) | Rust 原生 DLL 使用 Windows TextServices API 绑定 | Rust 系统输入法接入确有实例，不是只能使用 C++ |
| [Windows Chewing 更新记录](https://github.com/chewing/windows-chewing-tsf/blob/main/CHANGELOG.md) | 记录 COM 引用计数、上屏竞态、焦点与候选窗进程方案调整 | Rust 仍需兼容性设计；不能把所有 UI 强行移到外部进程当成通用正确结论 |
| [KeyMagic 3](https://github.com/thantthet/keymagic-3) | README 明确将性能、内存安全、跨平台作为 Rust 重写目标 | 这是作者写明的动机；较新项目不能用来证明行业普及率或成熟度 |

最后一列除特别注明作者明确表述外，都是基于公开结构的判断，不冒充原作者的历史选型理由。不开源的商业输入法完整技术栈没有可靠资料时不作猜测；不把项目数量当作市场统计。

## 平台与工具官方资料

- [Windows TSF](https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework)：COM / 文本服务与接入。
- [Windows IME 要求](https://learn.microsoft.com/en-us/windows/apps/develop/input/input-method-editor-requirements)：宿主加载、AppContainer、UI 与系统兼容边界。
- [Rust 平台支持](https://doc.rust-lang.org/rustc/platform-support.html)：各目标支持级别，不能把目标可构建等同于产品已验收。
- [Rust FFI](https://doc.rust-lang.org/nomicon/ffi.html)：跨语言边界与安全封装责任。
- [Rust 所有权](https://doc.rust-lang.org/book/ch04-00-understanding-ownership.html)：内存安全机制，不是死锁或全部逻辑错误保证。
- [微软 Windows Rust 绑定](https://github.com/microsoft/windows-rs)：系统 API 与 COM 绑定。
- [Android IME](https://developer.android.com/develop/ui/views/touch-and-input/creating-input-method)：InputMethodService、输入连接与界面生命周期。
- [Android NDK](https://developer.android.com/ndk/guides)：原生库复用与 JNI。
- [Apple InputMethodKit](https://developer.apple.com/documentation/inputmethodkit)：macOS 系统接入入口。
- [Apple 键盘扩展](https://developer.apple.com/library/archive/documentation/General/Conceptual/ExtensibilityPG/CustomKeyboard.html)：扩展、网络开放访问与不能使用第三方键盘的输入类型；归档资料，开发时复核当前 SDK 行为。

## UI 与数据候选资料

- [WinUI 3](https://learn.microsoft.com/en-us/windows/apps/winui/winui3/)：现代 Windows 设置 UI；不等于适合直接放入 TSF 宿主 DLL。
- [Jetpack Compose](https://developer.android.com/compose)：Android 现代 UI，实际键盘路径仍需性能和生命周期验证。
- [Qt Quick](https://doc.qt.io/qt-6/qtquick-index.html)：QML / C++ UI 与动画；是否采用需同时比较维护和打包许可。
- [Flutter 平台集成](https://docs.flutter.dev/platform-integration/platform-channels)：共享 UI 仍会涉及平台代码。
- [基础拼音词库](https://github.com/rime/rime-pinyin-simp)与[许可证](https://github.com/rime/rime-pinyin-simp/blob/master/LICENSE)：Apache-2.0 仓库，具体数据来源仍需追溯。
- [雾凇拼音](https://github.com/iDvel/rime-ice)：GPL-3.0-only 仓库，扩充词库应逐项核对。

调研项目是选型证据，不代表允许直接将其引擎或任意代码搬入本项目。实际引入以 [开源与分发](OPEN_SOURCE.md) 的清单和核对结果为准。
