# LIGHT8B accepted Flamestrike resource recipe

User feedback on 2026-10-07: the WD135A-compatible LIGHT8B package was downloaded and tested, with acceptable results. This is basic visual acceptance, not an FPS, load-time or terrain-wide qualification.

This change is a client M2/DBC resource recipe, **not a new renderer DLL or INI feature**. It preserves WD135A spell 9003953. Two burst flame emitters get taller particle quads, increased speed/lifetime and reduced emission rate; the non-glowing ground crack layer darkens; existing embers/smoke last longer with controlled rates. It does not implement vegetation combustion, permanent scorch marks or new ash textures. Existing sustained ground flame composition remains in place.

## Reproduce on Windows

Requires Python 3, pympq with its bundled StormLib.dll, and the exact original client baseline listed in baseline-sha256.json. Run:

```powershell
python tools/RebornFlamestrike/build.py --client-data "D:/your-blacknight/Data" --output "D:/new-LIGHT8B-output"
```

The output must be a new directory outside the client. Input archives are opened read-only. Any baseline hash mismatch fails rather than replacing newer unrelated DBC records. Use a backup of the accepted WD135A Patch-XA and original patch-Z if you have subsequently imported another patch.

The four input DBC tables and six M2/SKIN files are proprietary client inputs and are not in Git. The builder preserves all old DBC records except the visual-reference field on nine ordinary Flamestrike ranks, adds private visual/model references, and verifies MPQ content on readback. No server DBC is generated or needed for these visual changes. Changes and reference output hashes are included here.

## Installation

Use the original accepted package's instructions. Either install the generated patch-ZZ MPQ alongside the compatible client, or import client_mpq输入 at its original internal paths into a backed-up Patch-XA; do not use both. Keep backups outside Data. Renaming to patch-ZA changes potential archive precedence, so verify your custom loader. Close the game before installation.

LIGHT8B was tested with the local LIGHT7A loading-optimized lighting DLL. That separate DLL stage is not included or newly published in this commit; building the repository at this commit does not automatically produce it. The resource patch itself changes flame graphics independently of supplemental environmental light.

The later WD135UI1 patch changes Recall's icon and cumulatively retains these visual resources. Do not install the older LIGHT8B Spell table over that newer UI patch.

## Acceptance and provenance

Original archive: codexfix_20261007_092024_LIGHT8B_WD135A兼容火苗焦痕余烬.zip
SHA256: bc3a416cb52949e469d6ca92ec085b1d26bb1c43d3f6004b9d703c85e64937f0
Original 195 particle-track checks passed; mesh vertices/SKIN preserved; only nine existing Spell visual fields changed. All WD135A spell IDs and the string block remain intact. The old LIGHT8A package omitted WD135A 9003953 and is superseded.

M2 structure reference: https://github.com/Kelsidavis/WoWee/blob/master/src/pipeline/m2_loader.cpp
