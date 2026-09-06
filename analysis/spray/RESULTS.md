# 喷漆补丁验证记录

## 已确认

- 运行目标：Badlion Client `v4.4.4-f8775e4-PRODUCTION4 (1.8.9)`，JVM PID `28548`。
- 真实运行时注册表发现 253 个喷漆；商店目录 10 个，补全后 `i(SPRAY)` 为 253。
- `aFM.X(id)` 所有权判断：`0/40/169/252` 为允许，`-1/1000000` 为拒绝。
- Cosmetics 页面重新进入后显示完整 Sprays 目录和纹理；`U` 菜单可选新增喷漆。
- 通过 UI 将 Black Cat（ID 0）保存到第 2 槽，原 Badlion（ID 40）保留在第 1 槽；`Y` 轮盘显示两项。
- 在单人世界中按 `Y` 选择 Black Cat 后返回世界，目标方向出现喷漆位置的可见蓝色效果；这证明左键选择路径已实际触发。由于当前视角和距离限制，未把纹理近距离截图作为最终渲染证据。
- `analysis/spray/fixture-results.txt`：19 项离线合成 fixture 回归全部通过。
- `verify-artifacts.py` 已通过：发布 EXE 的 RCDATA 101/102 与当前 DLL、class 清单字节一致；测试 DLL 与正式 DLL SHA-256 一致。

## 尚未确认

- 尚未正常退出并从干净 JVM 重新注入发布版复现；当前 PID 已经注入，不应重复注入。
- 未验证服务端拥有状态或其他玩家可见性；补丁只修改当前客户端进程。
- 近距离方块表面纹理截图仍建议由用户在重启后完成一次最终验收。

## 复现

1. 正常保存退出 Badlion，重新进入 1.8.9 单人世界。
2. 运行 `dist/BadlionUnlockUI.exe`，点击“注入并解锁”。
3. 关闭并重新打开 Cosmetics，进入 `Your Cosmetics -> Sprays`。
4. 按 `U`，选空槽，再选 `Black Cat`；按 `Y` 打开轮盘并左键选择第 2 槽。
5. 将准星对准近处方块，左键选择喷漆；观察方块表面纹理。

## 证据文件

- `analysis/spray/runtime-evidence.txt`
- `analysis/spray/verification.json`
- `analysis/spray/spray-source-from-baseline.patch`
- `analysis/spray/ui/01-spray-catalog.png` … `04-wheel-black-cat.png`
- `analysis/spray/fixture-results.txt`
