# Microsoft C++ 标准库来源

本机MSVC 14.44.35207的 `yvals_core.h` 标注Copyright (c) Microsoft Corporation及Apache-2.0 WITH LLVM-exception。原生程序使用这些系统工具链头文件和标准库；没有把工具链或头文件复制进源码包。

`LICENSE.txt`按微软官方STL仓库的 `vs-2022-17.14` 标签固定下载。标签对象为 `94b3a6df7fa03423d2df5b936b2cfa5a8da243b2`，指向提交 `1f6e5b16ec02216665624c1e762f3732605cf2b4`；许可证SHA256为 `1fc6b6db121d4555d8f661937d7f6013693cd8fd67495d5e5d98c1d18fb83eda`，原始字节保留，详见SOURCE.json。

根NOTICE附完整许可证，使二进制预览包也保留此记录。该记录只覆盖Microsoft STL，不把其许可扩大到整个MSVC、Windows SDK或系统DLL。构建使用安装于本机的工具链，预览不附带编译器、SDK或VC Redistributable安装器。正式部署仍须使用合法授权的构建工具并满足对应分发条款。

核对：2026-10-23。依据：[固定许可证](https://github.com/microsoft/STL/blob/1f6e5b16ec02216665624c1e762f3732605cf2b4/LICENSE.txt)、[微软Visual Studio分发清单](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)。
