# 来源清单

核对日期：2026-10-22。第三方材料保留原许可；本目录不是“所有依赖均已可发行”的承诺。

| 材料 | 固定来源 / 用途 | 已完成与范围 |
| --- | --- | --- |
| AGPL-3.0-only 文本 | [SPDX 官方 license-list-data](https://github.com/spdx/license-list-data/blob/main/text/AGPL-3.0-only.txt) | `../LICENSE`；SHA256 `d8a6cc31abc16b6748c7a21f21611f5a1ec33f67d22ca23d7da1c19b95496bee`；只约束新写代码 |
| Rust 标准库 1.99.1 | 官方发行包随附 `COPYRIGHT-library.html` 和 `licenses/` | 完整库版权目录保存在 `rust-runtime/`，HTML SHA256 `5647be074c8edf7339fd863055923a8fc80bc5610a8d4661ec3b767b9d392c27`；随 Rust 二进制保留，后续每次升级重新核对 |
| rime-pinyin-simp 词表 | [commit 0c6861ef7420ee780270ca6d993d18d4101049d0](https://github.com/rime/rime-pinyin-simp/tree/0c6861ef7420ee780270ca6d993d18d4101049d0) | Apache-2.0；原始词表、许可、AOSP NOTICE 和转换记录位于 `pinyin-simp/`；65,125 条数据已接入 Rust 核心，没有引入 Rime 引擎 |
| AOSP PinyinIME NOTICE | [commit 49aebad1c1cfbbcaa9288ffed5161e79e57c3679](https://android.googlesource.com/platform/packages/inputmethods/PinyinIME/+/49aebad1c1cfbbcaa9288ffed5161e79e57c3679/NOTICE) | Apache-2.0，Copyright 2009 Android Open Source Project；SHA256 `b5830d96fb5a7e7e7ebcc295f352846b4b998e78fdc8f9aa68e134d2e4b39986`；固定提交重新下载比对一致 |
| Windows SDK / MSVC | 本机微软官方工具安装 | 使用系统接口和运行库；未复制 SDK 或打包 VC Redistributable，安装包分发审查待 P4 |
| C++/WinRT 2.0.250303.1 | Windows SDK 10.0.26100.0；Windows.Data.Json 系统 JSON API 投影 | MIT；许可及哈希见 `cppwinrt/`，编译进翻译程序的投影代码随发行保留微软版权及许可；不分发 SDK / WinRT 系统二进制 |
| Microsoft C++ STL | MSVC 14.44.35207，官方vs-2022-17.14许可证 | Apache-2.0 WITH LLVM-exception；固定原件和SHA256见 `msvc-stl/`，根NOTICE附完整许可随二进制预览保留；此项不扩大到整个MSVC/SDK |
| 原始品牌 PNG | 用户提供并确认由 GPT 生成，位于 `assets/branding/original/` | 已登记用于 TransInput；条款依据与品牌复用规则分别记录，见素材说明 |
| GitHub Actions checkout | v7.0.1，固定完整 commit；仅用于 CI 获取源码 | MIT；不进入输入法安装包；来源与工作流边界见 [CI.md](CI.md)，尚未在远端执行 |

当前基础词表已完成具体文件下载、来源记录、逐行校验、固定哈希和确定性转换，详见 [SOURCE.json](pinyin-simp/SOURCE.json)。原件 SHA256 为 `e341598343a0f0f2035bb1aafc34a7f3bb7887deeecb3f60796262aaa2983e6b`，转换后 SHA256 为 `288972b2014f16cd333c52e536608801ee327e50252f5f3c06677520ed8a8401`。

词条、拼音、整数词频全部保留；原 YAML 元数据转换为自研 TSV 格式，第三方数据仍采用 Apache-2.0，未改标为 AGPL。构建与分发需一并保留来源、上游 LICENSE 和 AOSP NOTICE。小词表现仅在 Rust 单元测试编译中使用。

这只是基础数据基线，未宣称现代口语、新词、专业词、人名和整句排序已达到日常输入质量；后续扩充数据逐项审查。Windows TSF 已通过独立后台查询真实词表，实际应用的可见键盘操作仍待验收。

构建工具属于本地 `.tools/`，不进入仓库或安装包。rustup 1.29.1 官方安装器 SHA256 为 `6f4bef66261261fcb43131be8720bab817d403a09edec7455c371974b90bdb7e`。本轮下载 Rust 组件时官方 stable 清单与 rsproxy 清单 SHA256 一致：`fb152be80975c194aa5fc684b2aec3a6e14e2b855efe2faa4e5f8ae6e590f463`，组件由 rustup 校验。`scripts/bootstrap-rust.ps1` 进一步固定到编译器 1.99.1。

项目没有复用旧版离线模型、个人词库或输入日志，没有将 API Key 打包或提交到源码。默认测试使用虚构凭据；经用户授权的独立九句真实接口测试只在内存解密旧版已保存的 Key，未迁移配置，见测试报告。未来导入任何外部数据 / crate 后，应在此逐项登记并生成实际依赖清单。
