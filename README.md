# Badlion 1.8.9 本地饰品解锁

开发者：ColdEternity Team

当前版本：`v1.3.0`

原始仓库：<https://github.com/LongCold-ColdEternity-Team/Badlion-Local-Cosmetics-Unlocker>

作者 QQ：`3442312505`

如果这个项目对你有帮助，可以点一个 Star，谢谢！

该工具只修改当前 Badlion Java 进程内的 cosmetics response/cache，不修改 `BLClient.jar`、账号数据或服务端拥有状态。目录中的原始 `aCV` 对象会被直接复用，因此模型和资源元数据保持完整；效果只存在于当前客户端进程。

## 使用

1. 通过 Lunar Client 启动 Badlion 1.8.9，等待游戏窗口出现。注入器会同时检查窗口标题、窗口类名以及 Java 进程路径，兼容标题变化、大小写变化、边框/无边框窗口和 Lunar JRE 安装位置变化。
2. 双击运行图形注入器：

   `dist\BadlionUnlockUI.exe`

   该版本已内嵌 DLL 和类清单，可以单独复制、运行。界面检测到游戏后，点击“注入并解锁”。

   命令行版本仍可用于调试：

   ```powershell
   .\bin\BadlionUnlockInjector.exe
   ```

3. 注入成功后，在游戏菜单打开 `Cosmetics`，进入 `All Cosmetics`。页面会读取完整目录，点击任意有资源预览的饰品即可在右侧本地模型显示。

## 喷漆支持（本地补丁）

喷漆补丁位于 `spray_support.inl`，由 `agent.cpp` 包含；这是构建必需的源码，分发源码时不能遗漏。补丁从当前进程的喷漆资源注册表补全目录，同时构造完整的拥有列表和按类型缓存。它不会将全部喷漆设为启用，也不会直接覆盖喷漆槽位。

在 `v4.4.4-f8775e4-PRODUCTION4 (1.8.9)` 上，运行时注册表有 **253** 个喷漆；此数量是当前版本的观测结果，不是硬编码的 ID 范围。商店已有条目及原拥有的、商店缺少的喷漆对象会保留，新增条目的 ID 和名称来自已加载的资源注册表。

使用方式：

1. 注入后，如果 `Cosmetics` 已经打开，请先关闭整个页面，再重新进入 `Your Cosmetics → Sprays`；旧页面可能缓存原来的目录。
2. 回到游戏按 `U` 打开喷漆配置，先选轮盘里的空槽位，再点右侧 `Available Sprays` 中的喷漆。使用自定义键位时以菜单显示为准。
3. 按 `Y` 打开轮盘，选择已保存的喷漆。这里的槽位使用 Badlion 当前 Mod Profile 自己的保存机制，与下文 `selection-v1.txt` 中的普通饰品启用状态不同。
4. 如果按 `U` 弹出输入法组合框而不是游戏菜单，先取消组合输入、切换为英文输入再按键。

2026-09-06 的验证范围：

- 已通过：真实拥有接口 `i(SPRAY)` 返回 253 项；槽位所有权校验接受样本 ID `0 / 40 / 169 / 252`，拒绝 `-1 / 1000000`。
- 已通过：重新打开的喷漆分类和 `U` 菜单显示新增喷漆及纹理；通过游戏界面将 Black Cat（ID `0`）保存到第 2 槽，原第 1 槽 Badlion（ID `40`）保留；`Y` 轮盘正确显示两项。
- **尚未通过最终验收**：方块表面的实际放置/世界渲染，以及重启客户端后只注入发布版的干净复现。不能把菜单出现、槽位保存或发送日志当作喷涂成功。
- 不代表服务端拥有状态发生变化，也不保证其他玩家可见。

目前运行中的 JVM 如果已经注入旧/测试 DLL，不要再重复注入新 DLL；请正常保存退出并重新启动游戏后再测试发布版。测试截图和精简记录见本地 `analysis/spray`（无需随单文件程序分发）。

## 本地选择保存

首次注入时目录会完整显示，但所有饰品默认关闭；只有配置文件中保存的选择会自动恢复。升级时如果检测到旧版本生成的“全目录已启用”配置，会自动清空该状态，避免再次把全部模型同时显示。

注入后，程序会自动记录当前处于启用状态的本地饰品。下次启动游戏并再次注入时，会按“饰品类型 + 饰品 ID”自动恢复，无需重新打开每个分类选择。

配置保存在：

```text
%LOCALAPPDATA%\ColdEternityTeam\BadlionLocalCosmetics\selection-v1.txt
```

选择变化后约 1 秒内自动写入。删除该文件即可清除已保存的本地选择；该文件不包含账号令牌或登录信息。

命令行调试版本也支持显式指定 PID：

```powershell
.\bin\BadlionUnlockInjector.exe --pid <javaw PID>
```

如果提示没有目标 JVM，先等游戏窗口完全出现再运行，并可点击“重新检测”或按 `F5`。若提示 `Agent already loaded`，说明当前 JVM 已经注入；重启游戏后可再次使用。

## 构建

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

默认产物：

- `bin\BadlionUnlockInjector.exe`
- `bin\blc_unlock_agent.dll`
- `dist\BadlionUnlockUI.exe`（内嵌依赖的单文件版本）

实际使用只需要复制 `dist\BadlionUnlockUI.exe`；`bin` 目录是构建和命令行调试产物。

运行日志写入注入 DLL 所在目录的 `blc_unlock_agent.log`。单文件 UI 会将 DLL 释放到 `%TEMP%\BadlionUnlockInjector`，点击“打开日志”时会自动定位该目录、程序目录及 `bin` 目录中的日志；若系统没有 `.log` 文件关联，会回退到记事本打开。该实现针对 Badlion Client `v4.4.4-f8775e4-PRODUCTION4 (1.8.9)` 的运行时类名和目录结构，客户端版本变化后需要重新定位字段/方法。

## 二次修改与分发

本项目采用自定义 Source-Available License，具体条款见 [LICENSE](LICENSE)。主要要求：

- 二次修改、改编、重新打包或衍生发布无需事先授权。
- 二改版本必须显著标注 `ColdEternity Team` 和本原始仓库链接。
- 二改版本发布可执行文件时，必须同时在公开仓库提供能够复现该程序的完整源码，不得闭源。
- 原版和二改版均不得收费、付费下载、会员解锁、捆绑付费服务或用于其他直接、间接商业获利。
- 不得删除或隐藏作者、许可证及原始仓库信息。

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=LongCold-ColdEternity-Team/Badlion-Local-Cosmetics-Unlocker&type=Date)](https://star-history.com/#LongCold-ColdEternity-Team/Badlion-Local-Cosmetics-Unlocker&Date)
