# Creature template lighting profiles

CreatureLights maps template Entry IDs (OBJECT_FIELD_ENTRY, descriptor byte offset 0x0c) to CreatureLightProfile sections. Unmapped entries and missing profiles fall back to CreatureLight. Profile Enabled=0 disables that template; global Enabled=0 disables all creature lights.
Per-profile radius, intensity, color, height and environment multipliers inherit global values when omitted. Distance, active-light limit and death fade remain global. Example IDs in the INI are commented and must be replaced with actual entries, not GUIDs or item IDs. All instances of a template share its profile.

Validation: Win32 Release compilation; production ResolveProfile helper override/disable/fallback/removal and 10000 unmapped IDs; equipment combination regression. No claim of game visual acceptance. LIGHT3A delivered DLL SHA256: 6c229ca97581e3aee2e3cfc5f4b828fd81ff19e9b00ae826a8f73011cc7f4698. The committed runtime sources and main INI values match that package (INI line endings normalized); binaries remain in the separate upgrade archive.

See [Chinese tutorial](reborn-lighting-tutorial.zh-CN.md). TestDaylight=0 is a deliberate diagnostic override, not automatic nighttime detection.
