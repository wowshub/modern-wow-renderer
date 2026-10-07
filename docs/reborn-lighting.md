# Reborn actor lighting / 人物与生物照明

Experimental client integration, based on upstream release18, commit 8a5e394ff96e3ecac7fadfa3770e7cf5590a2f2b.
Only enables actor reads for blacknight Wow.exe SHA256 `2d89cf4231fa27b6f4f9a5e1ba08c6473474d04d62421725eb99c50f34dd99c2` and matching function entry bytes. Other clients skip actor lights.

PlayerLight controls the player's base radius/intensity. EquipmentLights maps equipped item IDs to LightProfile sections. CreatureLight controls nearby NPC/creature lights. Existing scene lights remain supported; actor lights are excluded from native volumetric integration. F12 reloads GraphicsEffects.ini.

VS2022 v143 Win32 Release builds were tested. The user confirmed visible player ground illumination after tuning; this does not certify creature visibility, automatic day/night behavior, every client, or all equipment cases. GPU HAL testing is unavailable in the build environment. Creature lighting remains a candidate.

No game binaries or private runtime configuration are committed. Install the matching built d3d9.dll with GraphicsEffects.ini, keeping upstream data/textures. Back up both files first. The delivered upgrade archives remain separate.
