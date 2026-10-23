# CI 工具记录

核对日期：2026-10-22。只用于未发布仓库的自动检查配置，不属于输入法运行时或安装包。

2026-10-23更新：控制台清单扩至每架构12路径，新增UI元素协商、UIA契约、只读部署和注册回滚自检。对应本机完整72项退出0；当前10个嵌入PowerShell块语法解析成功，证据`artifacts/ci-checks/20261023-003319-596/`。未远端执行或做GitHub schema验证；本机Python没有yaml模块，不能把其失败尝试说成YAML验证通过。以下旧数量保留为历史记录。

- `actions/checkout` v7.0.1，固定 commit `3d3c42e5aac5ba805825da76410c181273ba90b1`；官方 [发布](https://github.com/actions/checkout/releases/tag/v7.0.1) / [提交](https://github.com/actions/checkout/commit/3d3c42e5aac5ba805825da76410c181273ba90b1)。MIT，Copyright 2018 GitHub, Inc. and contributors；[固定许可](https://github.com/actions/checkout/blob/3d3c42e5aac5ba805825da76410c181273ba90b1/LICENSE)。只通过工作流引用，不把 action 代码复制进本项目或安装包。
- 该 action 使用 Node 24，由 GitHub runner 承载；工作流只授予 contents:read，不持久保存 checkout 凭据。没有第三方安装 action、发布 token、DeepSeek Key、定时运行或自动发布步骤。
- Windows runner 选 `windows-2022`。官方 [镜像清单](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md) 在核对时包含 VS 2022 17.14 与 Windows SDK 26100。镜像会更新，不宣称整台 VM 已不可变固定；运行时另用项目脚本安装固定 Rust 1.99.1。

工作流仅覆盖双架构核心 / native 编译、词表、C ABI、学习、协议 / HTTP 环回与未激活的 COM 生命周期；不进行 TSF 桌面激活、截图、实际应用、系统安装或付费模型调用。它不能替代本机完整回归、原生编辑器、人工输入与长时验收。

2026-10-22个人词阶段已把 `words-smoke.exe` 加入控制台清单，现每架构8个可执行检查路径。当前工作流10个嵌入PowerShell块重新语法解析通过，记录 `artifacts/ci-checks/20261022-180930-493257/`。本机完整53项包含这些控制台路径，但未按远端干净镜像执行工作流，未执行YAML/schema校验，也未运行GitHub runner。

当前尚未创建 / 推送 GitHub 仓库，也未在 GitHub runner 实际运行，不能显示为 CI 已通过。仓库建立后需核对首轮输出与镜像工具版本；本地构建通过不自动视为远端通过。

后续词条内全简拼混写阶段，本机全套扩展为57项（`artifacts/p1/20261022-224541-344/`），两架构各46 Rust单元+6词表集成，包括原40与新增22项质量检查。工作流的cargo测试自动纳入这些核心用例；本次未改工作流，未重新运行远端、未增加TSF激活或付费调用。新TSF `--spelling` 和算法计时属于本机全套范围，不据此声明远端已执行。

本机检查记录：`artifacts/ci-checks/20261022-163918-967944/`。从当前工作流提取的两架构共10个 PowerShell 块通过语法解析；其中控制台 / 协议步骤在 Windows PowerShell 5 中按原文执行，两架构各7项通过、退出0，结果见 `verified-results.json`。Rust / native 构建与完整检查另见 `artifacts/p1/20261022-161952-214/` 的43项。未额外安装 YAML / GitHub schema 校验器，提取脚本不是 YAML 校验器；本机没有重现 GitHub 的干净镜像或验证 checkout action 的远端行为。
