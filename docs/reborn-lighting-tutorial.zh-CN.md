# 大白话：怎么调整人物、装备、NPC 和怪物的光

编辑客户端根目录 GraphicsEffects.ini。不要重复添加同名配置段。改完可按 F12 重新读取；换 DLL 必须退出游戏重启。

## 人物没拿火把时
[PlayerLight] 的 BaseIntensity 管亮度，BaseRadius 管范围。目前为 0.8 和 3.0。只嫌暗就提高 BaseIntensity，例如 1.0；圈太小再改 BaseRadius，例如 4.0。

## 两件照明装备
目前 2808=SmallTorch，范围 8；1172=LargeTorch，范围 20。
EquipmentMode=Strongest：选 radius²×intensity 最大的那一件。
EquipmentMode=Additive：最大范围加其他装备范围乘 AdditionalRadiusFactor。目前 0.5，所以一起戴是 20+8×0.5=24。改为 1.0 就是 28。MaxEquipmentRadius=40 是叠加上限。
叠加只增加范围，颜色与亮度继续取最强配置，避免越戴越刺眼。TestProfile 保持空白，才按实际装备判断。
增加任意装备：在 EquipmentLights 添加「真实物品ID=自定义名称」，再建 [LightProfile.自定义名称] 填 Radius、Intensity、Color。

## 全体 NPC 和怪物
在现有 [CreatureLight] 中改：
Radius=3.0
Intensity=1.0
Color=0.85,0.75,0.60

Radius 越大照得越远；Intensity 越大越亮；Color 是红、绿、蓝三个分量（0 到 1）。当前强度沿用之前排查生物光的测试配置，可按实测调低。
MaxDistance=35 表示人物附近多远范围内参与筛选；MaxActiveLights=16 表示最多选多少盏生物光。数量多时会优先选视野中离镜头近的，并非所有远处怪物同时亮。
生物照明还有着色器总贡献上限，防止密集怪物把整个场景照白，所以无限提高 Intensity 不会无限变亮。

## 单独让某一种怪物更亮、更大，或不发光
先查它的生物模板 Entry ID，可通过服务端 GM 信息或 creature_template 查询。不要填 GUID、刷新点 ID、物品 ID 或怪物名字。
ActorLighting.log 的 creatureEntries= 也会列出附近最多 24 种 Entry，辅助核对，但不会提供名字对应关系。

下面的 12345 只是示例，必须换成真实 Entry：
```ini
[CreatureLights]
12345=BossLight

[CreatureLightProfile.BossLight]
Enabled=1
Radius=6.0
Intensity=0.8
Color=1.0,0.30,0.10
```
这会让这个模板的全部怪物使用半径 6 的暖红光。其他怪物保持全局设置。
本包已放好 BossLight、GhostLight、NoLight 三个配置，但映射行前面有分号，默认不启用示例 ID。把示例改成真实 Entry，再删除那一行开头的分号即可。
想让某模板不发光：在 CreatureLights 写「真实Entry=NoLight」。NoLight 的 Enabled=0。
同一个 Entry 只写一次。配置名称大小写保持一致。不要在数值或配置名称后面追加行尾注释，说明请另起一行用分号开头。

专属配置可设置 Radius、Intensity、Color、HeightOffset、OutdoorDayMultiplier、OutdoorNightMultiplier、DarkInteriorMultiplier；未写的继承全局。全局 Enabled 是总开关，距离、灯数上限及死亡淡出仍统一控制。

## 建议验收
固定同一地点与镜头，先观察默认光，再给附近一个已知 Entry 绑定 BossLight，按 F12 对比；切 NoLight 应关闭该模板的光，其他模板仍亮；删除映射后恢复全局。另确认 2808、1172 单戴与同时戴的范围差异、切换地图、杀死怪物后的淡出。
