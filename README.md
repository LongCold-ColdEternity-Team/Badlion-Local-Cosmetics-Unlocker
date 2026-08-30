# Badlion 1.8.9 本地饰品解锁

开发者：ColdEternity Team

当前版本：`v1.1.2`

原始仓库：<https://github.com/LongCold-ColdEternity-Team/Badlion-Local-Cosmetics-Unlocker>

作者 QQ：`3442312505`

如果这个项目对你有帮助，可以点一个 Star，谢谢！

该工具只修改当前 Badlion Java 进程内的 cosmetics response/cache，不修改 `BLClient.jar`、账号数据或服务端拥有状态。目录中的原始 `aCV` 对象会被直接复用，因此模型和资源元数据保持完整；效果只存在于当前客户端进程。

## 使用

1. 通过 Lunar Client 启动 Badlion 1.8.9，等游戏窗口标题出现 `Badlion Minecraft Client v4... (1.8.9)`。
2. 双击运行图形注入器：

   `dist\BadlionUnlockUI.exe`

   该版本已内嵌 DLL 和类清单，可以单独复制、运行。界面检测到游戏后，点击“注入并解锁”。

   命令行版本仍可用于调试：

   ```powershell
   .\bin\BadlionUnlockInjector.exe
   ```

3. 注入成功后，在游戏菜单打开 `Cosmetics`，进入 `All Cosmetics`。页面会读取完整目录，点击任意有资源预览的饰品即可在右侧本地模型显示。

## 本地选择保存

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

如果提示没有目标 JVM，先等游戏窗口完全出现再运行。若提示 `Agent already loaded`，说明当前 JVM 已经注入；重启游戏后可再次使用。

## 构建

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

默认产物：

- `bin\BadlionUnlockInjector.exe`
- `bin\blc_unlock_agent.dll`
- `dist\BadlionUnlockUI.exe`（内嵌依赖的单文件版本）

实际使用只需要复制 `dist\BadlionUnlockUI.exe`；`bin` 目录是构建和命令行调试产物。

运行日志写入 `bin\blc_unlock_agent.log`。该实现针对 Badlion Client `v4.4.4-f8775e4-PRODUCTION4 (1.8.9)` 的运行时类名和目录结构，客户端版本变化后需要重新定位字段/方法。

## 二次修改与分发

本项目采用自定义 Source-Available License，具体条款见 [LICENSE](LICENSE)。主要要求：

- 二次修改、改编、重新打包或衍生发布无需事先授权。
- 二改版本必须显著标注 `ColdEternity Team` 和本原始仓库链接。
- 二改版本发布可执行文件时，必须同时在公开仓库提供能够复现该程序的完整源码，不得闭源。
- 原版和二改版均不得收费、付费下载、会员解锁、捆绑付费服务或用于其他直接、间接商业获利。
- 不得删除或隐藏作者、许可证及原始仓库信息。

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=LongCold-ColdEternity-Team/Badlion-Local-Cosmetics-Unlocker&type=Date)](https://star-history.com/#LongCold-ColdEternity-Team/Badlion-Local-Cosmetics-Unlocker&Date)
