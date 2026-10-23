# 英文整段上屏与Notepad++绘制

2026-10-23。用户在英文候选完整显示后按2提交，反馈Notepad++有快速逐字填充和闪烁；Windows记事本/Codex较不明显。没有把候选已显示后的提交过程当成模型流式输出，现有请求仍为 `stream=false`。

## 发现与修改

- 旧模块使用新增受控TSF写入历史测试失败：等待写锁的拼音光标更新会在英文Commit前先写入文档，证据 `artifacts/instant-commit/before-fix.txt`，不能将最终文本正确当作没有中间写入。
- 已就绪的英文和中英提交取代队尾未执行的拼音Update；不跨越前一个Commit或待解析候选屏障。最终文字仍通过一次标准TSF SetText写入并完成组合，不模拟按键或用剪贴板粘贴。
- 这类明确提交使用TF_ES_ASYNCDONTCARE，由TSF在允许时立即执行，不能立即取得写锁时仍安全排队。普通拼音更新、中文提交、后台查询和网络继续使用既有异步路径。
- Scintilla的公开实现把完整IME结果逐字符交给编辑器，和所述现象一致。首版只在同步调用期间合并绘制，用户实际反馈仍有问题，因此不能宣称该做法已消除闪烁。
- 第二版将绘制合并覆盖到后续消息帧：只匹配当前进程/当前线程、可见且未被外层停绘的Scintilla控件；使用服务自有消息窗口和16ms预算的低优先级定时器，在提交调用退出后恢复。繁忙消息循环中可能延后，不宣称16ms硬实时保证；文本本身仍一次写入，不被定时器分段或延迟提交。嵌套消息循环不提前恢复，失败/失焦/停用/服务销毁均清理；目标数量有界，不使用桌面绘制锁、跨线程等待或后台线程。
- 品牌TI、模式中英、浅色逐行候选、Shift和1/2/3/Enter含义保持。

接口依据：[微软编辑会话时序](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcontext-requesteditsession)、[WM_SETREDRAW及原状态保护](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-setredraw)。Scintilla参考：[官方Notepad++源码](https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/scintilla/win32/ScintillaWin.cxx)，只用于分析宿主行为，未复制其代码或将其作为输入引擎。本机文件版本报告8.14，不把当前上游源码当作该二进制完全对应的证明。

## 验证状态

x64定向构建、候选与绘制作用域检查、真实TSF上下文测试通过。包含嵌套/隐藏/异常恢复、等待写锁时无中间拼音、可写宿主同次回调完成英文、560字符固定虚构英文/双语各一次完整写入。固定语料仅由无网络翻译fixture返回，不调用真实DeepSeek；旧模块先失败，新模块通过。

首版完整检查 `artifacts/p1/20261023-174046-115/` 74/74通过（111源/数据、56产物）；预览ZIP SHA256 `22851147a784189b06b610b3c82606871a87d3ce8e9ea2450c03d7ce7678dda8`。实际更新 `artifacts/tip-update/20261023-175003-148/` 四文件、57安装文件核对、两架构系统加载及Verify通过，回滚夹具恢复原53文件及marker，配置密文/加密备份未变。

已打开首版独立新Notepad++窗口PID24004，并核对其加载模块528384字节；用户回复 **“仍有逐字填充或闪烁”**。人工验收失败已记录 `artifacts/instant-commit/first-user-acceptance.json`，该反馈不能推给未加载新版，也不能用74项替代实际改善。首版证据以first前缀保留。

第二版消息帧修复的完整检查 `artifacts/p1/20261023-180317-371/` 74/74通过（111源/数据、56产物）；包含提交回调退出后仍保持绘制合并、嵌套消息循环不提前恢复、定时恢复、失败/隐藏/服务销毁恢复，以及原即时完整写入回归。

第二版预览ZIP SHA256 `41f2536dd9a565ed1f8a8a81e812520651c4dfb39b1a15be5e254b610a1953a5`。实际更新 `artifacts/tip-update/20261023-181230-246/` 四文件、61安装文件核对、两架构系统加载及Verify `artifacts/installation/20261023-181234-652/` 通过；回滚夹具恢复原57文件和marker，配置密文/备份哈希保持。已打开新独立Notepad++窗口PID13876、“TransInput-整段上屏验收-181242.txt”，请求复验整段上屏、闪烁和后续中文输入；反馈待收到。此时不能宣称实际问题已解决。
