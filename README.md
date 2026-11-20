# DebugNG

In-game event and hitch logger for Skyrim SE / AE - an SKSE plugin.

**Nexus Mods:** https://www.nexusmods.com/skyrimspecialedition/mods/199245

Big load orders stutter, lose player controls or break in ways that no regular log explains. DebugNG records what the game and your mods are doing while you play and writes it into its own log file:

```
Documents\My Games\Skyrim Special Edition\SKSE\DebugNG.log
```

## Features

- **Hitch detection** - frames clearly slower than the frames around them are logged together with the Papyrus scripts running at that moment (grouped and counted) and, optionally, the references attached to the world in that frame by plugin. Loading screens and menus are ignored.
- **Player control watch** - every change of disabled controls (movement, fighting, POV switch, sneaking, jumping, activate, menus, console) with the script call chain that caused it.
- **ModEvents** - `ModEvent.Create` / `Send` with the creating script, `Form.SendModEvent` with arguments and sender.
- **Game events** - menus, cells and locations, combat, death and bleedout, quests, equip, activate, furniture, sleep/wait/fast travel, model updates, tracked stats.
- **Chatty modules** (off by default) - magic, hits, item transfers, object load and cell attach (with base object and plugins), actions, script init, open/close.
- **Load order and SKSE plugins** at startup and on demand.
- **Log levels** - errors only / important / normal / verbose; modules that cannot write at the chosen level do not run.
- **In-game menu** (SKSE Menu Framework): live counters, marker button, settings, emergency page with "Give controls back" and "Player state to the log" (control locks, input context stack, camera, animation graph, active effects, scripts).

Settings: `Documents\My Games\Skyrim Special Edition\SKSE\DebugNG.ini` (created on first start, English comments).

## How it works

- Frame timing: hook on `PlayerCharacter::Update` (vtable 0xAD); the first frame after menus/loading is not counted.
- Papyrus stacks: read from the VM's running stacks under the VM lock (SEH-protected).
- SKSE object storage (ModEvents): found at runtime through SKSE's RTTI - no hard-coded SKSE addresses.
- ControlMap fields that moved in 1.6.1130+ are read with version-dependent offsets.

## Requirements

- SKSE64 matching the game version
- Address Library for SKSE Plugins
- SKSE Menu Framework (optional, in-game menu)

Skyrim SE 1.5.97, AE 1.6.x and 1.7.104 (SKSE 2.3.x, Address Library for 1.7). Skyrim VR: not tested.

## Building

Requirements: Visual Studio 2022 (C++ workload), CMake 3.21+, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

1. Put `SKSEMenuFramework.h` into `include/` (see [include/README.md](include/README.md)).
2. Open the folder in Visual Studio 2022, select the `release` preset and build (`Ctrl+Shift+B`) - or from a developer prompt:
   ```
   cmake --preset release
   cmake --build --preset release
   ```
3. The plugin is written to `build/release/DebugNG.dll`.

Dependencies come from vcpkg: [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) (multi-target SE/AE), spdlog, fmt.

## Credits

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) by CharmedBaryon and contributors, based on CommonLibSSE by Ryan McKenzie and powerof3
- SKSE team
- meh321 for Address Library for SKSE Plugins
- SkyrimThiago for SKSE Menu Framework
- spdlog (Gabi Melman) and fmt (Victor Zverovich)

## License

[GPL-3.0-or-later](LICENSE) - built with CommonLibSSE NG (alandtse), which is GPL-3.0-or-later.
