# Windows 输入指示资源

`input-indicator.ico`由本项目新写的几何绘制代码 `platforms/windows/resources/generate_icon.cpp`确定性生成，包含16/20/24/32/40/48px、32位alpha的黑白“TI”字样（2026-10-23按用户要求替换此前“中→”指示）；源码与本资源采用AGPL-3.0-only。未复制字体、现成输入法图标或第三方图标包。

此资源用于微软要求的单色输入指示，并嵌入原生DLL/EXE；应用设置仍采用用户提供的彩色GPT图标，原件不变。不是对用户PNG的修改或覆盖。实际托盘、缩放和读屏可见效果仍需正式签名部署验收。
