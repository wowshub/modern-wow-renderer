# 装备范围叠加

[PlayerLight] 中 EquipmentMode=Strongest 保持原选择（radius²×intensity 最大）；Additive 采用最大半径加其他半径乘 AdditionalRadiusFactor，并受 MaxEquipmentRadius 限制。
本配置2808半径8、1172半径20、AdditionalRadiusFactor=0.5，同时装备半径24；系数1时28。只叠加范围，亮度、颜色和闪烁取最强配置。基础亮度提高到0.8。TestProfile 保持空白。
VS2022 Win32编译和生产Combine函数10000个组合回归通过；新增叠加游戏画面仍待验收。自动昼夜限制延续。
