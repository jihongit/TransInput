# Windows 输入指示资源

`input-indicator.ico`由本项目新写的几何绘制代码 `platforms/windows/resources/generate_icon.cpp`确定性生成，包含16/20/24/32/40/48px、32位alpha的黑白“TI”字样（2026-10-23按用户要求替换此前“中→”指示）；源码与本资源采用AGPL-3.0-only。未复制字体、现成输入法图标或第三方图标包。

品牌资源1保留TI；同一几何生成器另生成 `input-mode-chinese.ico`（资源2，中）和 `input-mode-english.ico`（资源3，英），各含六种尺寸，白色字形、半透明黑描边及透明背景。模式切换时更新图标、文字和提示，避免两个位置都显示TI。没有复制字体或第三方素材。

资源嵌入原生DLL/EXE；应用设置仍采用用户提供的彩色GPT图标，原件不变。不是对用户PNG的修改或覆盖。资源预览/组件检查与实际Windows任务栏人工验收分别记录；正式签名部署仍为后续发行门槛。
