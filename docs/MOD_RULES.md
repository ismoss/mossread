# MOD_RULES · 二改最高权限规则

> 优先级高于一切功能目标。任何会话（人或 AI）在本仓库工作前必须先读本文。
> Priority above every feature goal. Any session (human or AI) must read this file first.
> 2026-10-24 用户授予最高权限设立。/ Established with top priority by the user on 2026-10-24.

## 1. 个人信息零提交（绝对）

公开仓库（ismoss/mossread）**禁止出现任何个人信息**，包括但不限于：

- 真实邮箱（git 作者/提交者邮箱一律用 `ismoss@users.noreply.github.com`；本机 `git config user.email` 已设，勿改回）
- WiFi SSID/密码、设备标识、串号、位置、姓名、手机号、QQ 号
- TF 卡内容、设备日志原文、配置备份文件（`Pico-settings.backup`）
- 任何含上述信息的截图/文档/临时文件

发布到 Pages 的产物（update.json / dns.json / 固件 bin）只允许：版本号、通用文案、公共 DNS 地址、SHA256。

每次发布前自检：`git log --format="%ae %ce" | sort -u` 不含非 noreply 的个人邮箱。

## 2. 功能三问（实现前必答）

做任何功能前先在 HANDOFF.local.md 或提交说明里回答：

1. **能不能实现**（技术路径、依赖、体积/内存预算——OTA 槽 4MB，当前余量见 idf.py size）
2. **会不会引入新问题**（功耗、残影、刷新性能、与现有功能的交互、上游合并冲突面）
3. **怎么回退**（设置开关？版本回滚？TF 刷回？）

答不清楚不开工。

## 3. 版本与发布

- 设备版本号：`X.Y.Z-rcN-modM`（官方基数 + 二改序号；rc203 之后启用，二改代际优先于 rc 数值）
- 发布走 GitHub Actions（`mod-build.yml` dispatch，版本号+说明），发布后线上自检自动执行
- 首次真机验收未过的功能不发布

## 4. 仓库结构（拟议，待用户确认后实施）

- 公开仓库 = 发布源（Pages feed）+ 精简代码，不放任何开发过程记忆
- 开发/记忆/交接若迁私有仓库，本文件随之复制并保持最高优先级
