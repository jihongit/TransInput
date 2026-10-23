# Shift切换、浅色候选与TI标识

2026-10-23。用户要求明确更换DeepSeek API Key的页面、单按Shift切换中英文、浅色候选窗，并将Windows输入指示“简体”改为TI。本轮在此前已获用户确认的紧凑布局上实施，旧布局验收不继承为新功能验收。

## 用户操作

托盘TransInput图标右键“设置”或双击打开现有设置页，选择“翻译”，在密码输入框替换API Key后保存。测试连接是可选、由用户主动发起的联网操作。本轮只请求显示已安装设置页，没有读取/输出Key或代为更改真实配置。

单独按下并松开左/右Shift，切换中文与英文，再按一次回中文。正在组合中文时先按所选中文完成，再进入英文；字母按系统大小写规则处理，Caps Lock仍保留。组合键、按键重复、双Shift同时按、失焦/上下文变化和只读输入框不会误切换。模式保存在当前TSF实例，不宣称所有应用全局同步。

候选窗沿用460×198逻辑像素和28行高，背景改为近白色、选中行为浅蓝色、正文为深色。仅选中中文旁显示英文。TI指示由本项目几何代码生成黑白16/20/24/32/40/48像素资源，不修改原始彩色GPT图标。

![浅色候选示例，固定虚构文本](../images/light-candidates.png)

![TI指示资源48像素预览](../images/TI-indicator.png)

## 接入与验证边界

- Shift在松开且未参与组合时切换；模式切换作为有序组合边界进入原队列，候选或写锁延迟时也不合成系统按键。已提交的候选后边界为空操作，避免额外空格。
- 输入法通过官方ITfLangBarItemButton/ITfSource提供TI文字、图标及当前模式提示；宿主引用保留模块lease，停用后回调失效并释放服务关联。
- 注册助手只增加本项目的SYSTRAYSUPPORT类别；没有为了图标声明尚未验证的IMMERSIVESUPPORT能力，不改公司策略或其他输入法。Windows Shell是否在现有会话立即显示TI仍需实际验收，不能以资源预览代替。
- 维护更新扩展为两架构TIP DLL与对应注册助手四个文件，保留已加载旧映像；更新失败时回滚文件和本次新增的品牌类别。正式签名门槛仍保持，未创建开始菜单项。

微软接口依据：[输入法与输入指示要求](https://learn.microsoft.com/en-us/windows/apps/develop/input/input-method-editor-requirements)、[ITfLangBarItemButton::GetText](https://learn.microsoft.com/en-us/windows/win32/api/ctfutb/nf-ctfutb-itflangbaritembutton-gettext)。通用语言缩写由Shell控制，不等同于本项目彩色图标文字。

定向x64新Shift/混输与TI资源/按钮组件检查通过；浅色与TI预览已查看。构建期间补齐了按钮连接点声明和预览工具的系统图形库链接，失败日志保留在artifacts/shift-light，不记为通过。自动回归无新真实DeepSeek调用。

## 完整回归、本机更新与人工复验

`artifacts/p1/20261023-161005-957/`，scope=all、x64/x86，74/74退出0，108源/数据和56直接产物快照。每架构46个Rust单元与6个真实词表集成通过。两架构Mixed检查包含新Shift边界：重复/双Shift/组合键/失焦/只读、有序中文到英文再到中文、数字待选后不多空格、候选/写锁延迟、原Caps混输和无英文模型请求；完整TSF/UILess/无障碍/原生编辑器/10秒资源检查通过。TI按钮的文字、图标、动作与断开后不可调用已覆盖，DPI预览与32轮稳定展示通过。不是所有应用、所有快捷键或8小时压力结论。

完整预览包 `artifacts/delivery/20261023-161930-477/TransInput-0.1.0-preview-windows.zip`，SHA256 `992a11facf1500ceb6f198cbd61c5500bd5ee89e7cf5436cac5b2e6c706207b3`。工作区回滚夹具 `artifacts/shift-light/rollback-20261023-161931-295/` 验证四文件置换后保护路径拒绝，原41文件/安装标记恢复，无暂存残留；实际安装和类别未改变。这不是新增类别后的失败注入验收。

实际更新 `artifacts/tip-update/20261023-161939-158/`，两架构TIP DLL与注册助手四文件替换，SYSTRAYSUPPORT仅由x64首次新增、x86幂等保留。45个安装文件校验、两架构实际系统注册COM与独立进程TSF激活通过；安装Verify `artifacts/installation/20261023-161941-139/` 成功。共享后台和设置程序不变，设置密文与加密备份哈希不变，不改默认输入法、微软拼音、策略或开始菜单，不强制关闭用户进程。

新TIP SHA256：x64 `df9756dc70d06bf08dda7429489b7c09346ff1145de96b8a1f3bb469d518f777`；x86 `136e132b882a13ae14299aa4fdf83fad5b08544095f003a809d126c4c5f22e02`。两架构注册助手对应哈希见本轮update-plan及安装记录。

用户首先回复TI“已显示 TI”；询问在哪里测试Shift后，已打开Windows自带记事本新进程（运行且响应正常，无自动按键输入），说明选择系统安装版TransInput、关闭Caps Lock，按 `nihao → Shift → abc → Shift → ceshi＋空格` 复验。用户随后明确回复**“文字正确，浅色且流畅”**。本轮代表性Shift切换、浅色界面、顺序与TI显示人工通过，不扩大为所有应用/长时或更换真实Key联网测试均通过。没有要求用户重复旧紧凑布局验收。仓库继续私有，公开仍需明确批准。
