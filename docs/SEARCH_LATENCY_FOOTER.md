# 搜索耗时底栏（Search Latency Footer）

「秒出」的查询不占地方；一旦不是秒出，窗口底部出现一条 slim 底栏，实时报**已经多少毫秒**，
让等待可见、可诊断，而不是对着空列表猜卡没卡。

## 何时出现 / 报什么

阈值 `kSlowMs = 120`（src/main.cpp）：低于它视为秒出，底栏不出现、窗口不长高。

| 状态 | 判定 | 底栏左 | 底栏右 | 颜色 |
| --- | --- | --- | --- | --- |
| 还在等（pending） | `SearchPending()` 且 elapsed ≥ 阈值 | `… {elapsed} ms` 实时跳 | 还在等哪层（`Windows Search 未回` / `Flowtary 索引未回` / `workshop 目录未回` / `应用列表加载中`） | 左 accent2（活动色） |
| 等完但慢（settled） | 报齐且 `qSettledMs` ≥ 阈值 | `{qSettledMs} ms` 定格 | `首屏 {qFirstMs} ms`（若首屏 < 总耗时） | 左 textSec |
| 秒出 / 空输入 | elapsed < 阈值 或 `text` 空 | 不出现 | 不出现 | — |

- `SearchPending()`：`Mode::Files && !dfWsDone`（WS/索引/workshop 异步层未回），
  或 `Mode::Programs && startingUp`（应用扫描未完成）。
- 时间基准复用搜索台账的 `g.qT0`（防抖后真正派发那一刻），与 Q 行的 firstMs/totalMs 同源。
- `qSettledMs` 在**第一次报齐**时由 `NoteQuerySettled()` 定格；后续重排/补行不刷新它。
  换查询（`ArmHistoryClock`）与唤醒（`Show`）复位为 -1。
- 实时跳毫秒靠 `kTimerLatency`（33ms ≈ 30fps）：pending 期间每次 tick 重新 `Layout()+Repaint()`
  （跨阈值时底栏出现/长高需要重排）；报齐即 `KillTimer`（定格数字不需要重画）。
  `Hide()` / `Show()` 也会停表，避免后台空转。

## 布局与绘制

- `Layout()`：`H = S(kBaseInputH + rows*kBaseRowH + (footer ? kBaseFooterH : 0))`，
  `kBaseFooterH = 22`。底栏出现/消失会改变窗口高度（centerWake 下整体重新居中）。
- `Paint()`：结果行之后、描边之前画底栏：顶边一条半透明分割线（`PaintDivider` alpha 90），
  左/右文本用字体池最小号 `FontSlot(kFontMin)`，`TRANSPARENT` 底。不遮挡任何结果行。

## 验证

`tests/latency_footer_ui_test.cpp`（ctest 名 `latency_footer_ui`）包含生产 main.cpp，
驱动真实 `Layout()+Paint()` 画进 DIB：

- 数据臂：pending 时 `LatencyFooter` 返回 live 且 ms 随 `Sleep(60)` 递增；
  pending 不定格、报齐后 `qSettledMs` 定格 ≥ 种子 elapsed；
- 像素臂：pending 慢 ⇒ 窗口长高一个底栏、底栏有 accent2 实时字 + 未回层标签；
  pending 快 ⇒ 窗口干净无底栏墨色；settled 慢 ⇒ 定格总耗时 + 首屏延迟、非实时色；
  settled 快 ⇒ 干净；
- 对照臂：空输入（清空结果）⇒ 无论耗时都不显示、窗口只剩输入框。

BMP 落盘供肉眼复核，转 PNG 见 build/latency_{pending,fast,settled,control}.png。

Mutant 自证：`bash tools/mutant_latency_footer.sh`。build/mutant 副本里先跑逐字节相同的
控制副本（必须全绿），再把 pending 分支阈值条件行 `return ms >= kSlowMs` 改成
`>= 1000000`（只改条件行），门必须红在 `FAIL: pending slow reports live elapsed`；
两腿任一不符即脚本退出 1。不碰共享树源码。