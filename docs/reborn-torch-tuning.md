# 火把分级配置

2808 → SmallTorch，半径8；1172 → LargeTorch，半径14。只放背包不生效，要装备。
在 GraphicsEffects.ini 的对应 LightProfile 中改 Radius 调范围、Intensity 调亮度、Color 调颜色。TestProfile 必须留空。
本阶段保留 TestDaylight=0 强制夜间的诊断配置；-1 才读取场景昼夜，blacknight 的自动值尚待核实。
玩家已反馈2808范围可用、希望1172更大；后续叠加批次调整1172与基础亮度。不是NPC专项验收。
