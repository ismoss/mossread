# 与上游对齐 · rc105 评估与分阶段移植

> 文档性质：工程决策记录（不是路线图）。路线图在 `ROADMAP.md`，验收/串口状态在 `HANDOFF.local.md`。
> 2026-11-11 评估，基于 `v0.3.3-rc100`（我方基线）与 `v0.3.3-rc105`（上游）。

## 一、结论先行

1. **不能 merge，也不能整包覆盖。** 我方 `mod/glass-dock-lockoff` 与上游全无共同祖先
   （`git merge-base` 返回空；我方 387 个提交，根提交不同），历史不可对接。
2. **rc105 不是"增量修复"，是一次结构性重构。** 仅 `components/read_pico/` 一次就
   拆走了 1709 行：`read_pico_board.c`(796)、`read_pico_sd.c`(575)、`read_pico_epd_timing.c`(177)、
   `read_pico_init.c`(161) 被拆到 `components/kiiko_board/`、`boards/pico/`、`read_pico_sd_access.c`。
   `main/display.c` 同样被整体拆掉（-1004 行）。我方的 120 个 mod 有大量落在这两类文件上。
3. 因此唯一可行路径：**按行为逐项移植，每次一个可编译、可验收的最小单元**。

## 二、量化

| 集合 | 文件数 | 说明 |
|---|---|---|
| 上游 rc100→rc105 改动的代码文件 | 650 | 含 S31（Metalio47）双板内容 |
| 两边都改过（冲突面） | 216 | `app_book.c`/`ui_kit.c`/`settings.c`/`CMakeLists.txt` 等主力文件 |
| 只上游改（可整体取用） | 433 | 其中 119 个是 S31/board，与 Pico 无关 |
| 我方 rc100 以来改动 | 300 | 120 个 mod 的落点 |

## 三、关键发现：rc105 内部已经包含「第三方分叉的痛点修复」

Pico-Plus（`cere-hy/pico-plus`，基于 r95）对外宣传的若干改进，上游 rc105 里已有对应实现，
**不需要逆向他的固件，也不需要他的源码**：

| 外部宣称 | rc105 对应实现 | 我方状态 |
|---|---|---|
| 西文按词断行（Liang 断词） | `main/book/english_hyphen.h` + `english_hyphen_patterns.h`（135 KB 表）+ `book_layout.c` 接入 | 阶段 1 已移植「词界回退」，Liang 软连字符表留待阶段 4 |
| 界面切换跟手 / 更快 | `main/ui/ui_follow_cache.h`/`ui_follow_pixels.h`/`ui_follow_scroll.h` + `ui_glass.c` | 未移植 |
| TF 卡热插拔 | `components/read_pico/read_pico_sd_access.{c,h}` + `read_pico_sd_msc.h` + `main/app/sd_recovery.{c,h}` | 未移植（注意：`read_pico_sd_start_recovery()` 我方 rc100 已有） |
| 页面缓存，减少重绘 | `main/book/reader_page_cache.c` | 未移植 |
| 首页/书架刷新更快 | `main/apps/app_dashboard.c`、`main/ui/ui_home_widget.c` | 未移植 |
| 顶部下滑快捷菜单 | `main/ui/ui_quick_menu.c`/`ui_quick_menu_input.c` | 未移植 |
| 内存可观测 | `main/app/local_latency_probe.h`、heap 归因日志 | 未移植（Pico-Plus 的 `heapdiag` 同思路） |

另有 rc105 全新增量能力（我方都没有）：`online_font*`（在线字体）、`kiiko_locale`（多语言）、
`app_onboarding.c` + `setup_state.c`（首次开机引导）、`ui_ai_menu.c`、`ui_wifi_qr.c`、`book_added.*`。

## 四、分阶段移植计划

排序原则：先"行为可验证、改动局部"的，后"跨模块重构"的。每阶段一次发布、一份真机验收清单。

| 阶段 | 内容 | 落点 | 风险 | 状态 |
|---|---|---|---|---|
| P0 | 对齐账本（本文档） | `docs/` | 无 | ✅ 2026-11-11 |
| P1 | 西文按词断行（词界回退，不含 Liang 表） | `main/book/book_layout.c` | 低：只影响纯 ASCII 词，汉字/竖排不变 | ✅ mod121 |
| P2 | 电量独立刷新节拍 + TF 升级包双文件名 | `app_home.c`/`app_book.c`/`ota_update.*` | 低：纯行为增量 | 待做 |
| P3 | heap 归因观测（内存体检骨架，定位"每开一本书吃 1KB"） | `main/app/` + 设置页 | 低：只加观测不加策略 | 待做 |
| P4 | Liang 软连字符（表源 136 KB，实占约 56 KB） | `main/book/english_hyphen*` + `book_layout.c` | 中：分页变化 | ✅ mod125 |
| P5 | TF 访问守卫（`read_pico_sd_access`） | `components/read_pico/` | **阻塞**：上游删掉了 `read_pico_sd.c`(-575)，其余搬去 `kiiko_board`；我方 rc100 已有 `read_pico_sd_start_recovery()` 与 app_sd 重读入口 | 不做整包对齐 |
| P6 | 跟手滚动（`ui_follow_*`） | `main/ui/` | **需重写**：新头文件 + 我方 `ui_kit`/设置页/文件页滚动全要改，无真机不可验 | 暂缓 |
| P7 | 阅读页缓存（`reader_page_cache.c`） | `main/book/` | **需重写**：与 mod86 内存策略、prefire 交互 | 暂缓 |

**明确不取**：`boards/metalio47/*`、`*metalio*`、`components/epdiy/src/riscv_pixels.c`、
`read_pico_sd.h` 里 `#if KIIKO_BOARD_METALIO47` 的分支——本机只有 Pico（ESP32-S3），
引入 S31 代码只增加 flash 占用与编译面。

## 五、验证口径（每次发布都要写清）

- 编译：本机 `idf.py build` 零告警（多 1 条即新引入）；
- 宿主回归：`tools/run_book_management_tests.sh`（CI 里为 Linux 全量）；
- 真机：本轮改动都是排版/行为层，**必须真机肉眼**验证，代码级推演不算。

## 六、2026-11-11 复盘：为什么 P5–P7 不能"取文件"

对 P5–P7 做过逐文件核对，结论是**它们都不是"拿一个文件过来"能解决的**：

- **波形/显示（"页面停留变黑"方向）**：上游 rc105 在 `components/epdiy/` 里删掉了
  `epdiy.c`(-543)、`lcd_driver.c`(-853)、`render.c`(-680)、`lut.c`(-474)、
  `render_context.c`(-110)、`render_lcd.c`(-304)、`epd_board.c`(-120)，另加 `riscv_pixels.c`(+293)
  ——那是 S31（RISC-V）像素管线，Pico 侧文件被搬去 `boards/pico/`。整包取过来等于删掉我方驱动。
- **真正修"静止白底持续受驱动"的是上游 rc95 那条**：`结束时统一释放驱动、短暂 I2C 失败有界重试`。
  **我方 rc100 基线已经包含**，证据是 CI 里这条宿主用例长期 PASS：
  `idle/no-change drive release, zero redundant I2C and retry after failed neutral hardware writes`。
  也就是说这条痛点在我方不是"没修"，若真机上还看得到，那是另一条路径（缓存帧/刷新模式），
  要按不变量 #6「先确认这一帧从哪来」排查，而不是搬波形。
- **TF 访问守卫**：同一次重构把 `read_pico_sd.c` 拆走，守卫代码依赖新的 `kiiko_board` 抽象。
- **跟手滚动 / 阅读页缓存**：新文件是照着他们重构后的 `ui_kit` / `app_book` 写的，
  我方这两个文件各有几十个 mod（锁屏方向、划线、文件页套件、内存策略），照搬等于重写。

## 七、仍然可选、且不需要真机就能判断的候选

| 候选 | 为什么没做 | 若要做的落点 |
|---|---|---|
| 「默认全屏」开关（上游 rc105 明确语义：只影响新开书、默认关、睡眠恢复保留记录） | 我方阅读设置页 8 个槽位已满（0–7 全占用，且四行双列刚好卡在「完成」之上），加第 9 个开关要改版式；且我方现状是"进书即全屏"，默认值应取 **开** 才不改现有行为 | `settings.c` 加键 + `app_book_request_open()` 的 `s_requested_fullscreen` + 设置页槽位重排（或挪到 设置→显示） |
| 阅读字体后台准备（`reader_font_worker` / `reader_font_reload_tick`） | 需要新增工作线程与旧字库回滚，牵动 mod31 的字体事务 | `app_book.c` + `ttf_font.c` |
| 书架分页箭头按下/抬手归属 | 纯交互打磨，收益低于改版式风险 | `app_book.c` 书架分页命中 |
