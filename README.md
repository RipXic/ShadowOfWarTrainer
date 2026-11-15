# Shadow of War Trainer

An in-game trainer for **Middle-earth: Shadow of War** (single player), with its own overlay panel. Press
**Insert** in game to open it.

> Works with the Steam version **1.0.8874** (the current Steam build). On any other version the trainer still opens,
> but switches every feature off by itself.

## Features

| Tab | What it does |
| --- | --- |
| Player | Infinite Health, No Summon Cooldown, Heal Now, Infinite Focus / Might / Elf Shot / Elven Rage, Might Never Drains, Elven Rage Refills After Use, Wraith Chain tweaks (chain without aiming, no slow-motion, fast chain) |
| Skills | Level, Skill Points, Mirian, Give Item, Choose Multiple Skills, Unlock All Skills in the Menu, Activate All Upgrades, Raise the Dead (Dark Talion), Prestige for every gear slot |
| Gear | Edit any item: level, effects (add / replace / remove, values), look (transmog with preview), rarity / type, Auto-Complete Gear Challenges |
| Inventory | Copy or remove gear, add any captain gear, add an edited item |
| Loot | Choose what captains drop and how often, or make them drop an edited item |
| Army | Edit followers (level, intel, bodyguard, upgrades, gear, role, name, rarity), Instant Dominate, Dominate Captains at Full Health, recruit any orc, create new orcs |
| World | Infinite Mission Time |

Options you switch on are remembered and switched on again once you are back in the game world.

## Using it

1. Download the release zip and unzip the `ShadowOfWarTrainer` folder anywhere (keep its files together).
2. Run `ShadowOfWarTrainer.exe`. If the game is not running it starts it through Steam and attaches by itself; if it
   is running it attaches right away.
3. In game press **Insert** to open or close the trainer (mouse, keyboard or gamepad).

**Back up your saves first** - gear, level, skill and item edits are saved into your game.
Steam saves: `C:\Program Files (x86)\Steam\userdata\<your id>\356190\remote`. Single player only.

Nothing in the game folder is changed. To remove it, close the game and delete the folder.

## Building from source

Requirements: Windows 10/11 x64 and Visual Studio 2022 (or newer) with the *Desktop development with C++* workload.

```bat
build.bat
```

The script finds Visual Studio with `vswhere`, builds MinHook and Dear ImGui once, then the trainer. Output:
`build\ShadowOfWarTrainer.dll` and `build\ShadowOfWarTrainer.exe`. Put both next to the contents of `data\`
(`panel.json`, `signatures.json`, `config.json`, `skin\`, `scripts\`, `data\`) to get a runnable folder.

## Layout

| Path | Contents |
| --- | --- |
| `src/` | The trainer DLL: overlay and panel (`overlay.cpp`, `panel.cpp`, `skin.cpp`, `ui_kit.h`), core helpers (`core.cpp`, `calls.cpp`, `sigs.cpp`), the command table (`server.cpp`) and the game features (`sow*.cpp`) |
| `loader/` | `ShadowOfWarTrainer.exe`: starts the game if needed and attaches the DLL |
| `data/` | Files shipped next to the DLL: the panel layout, byte signatures, skin art, the game-script helper and name lists |
| `third_party/` | [MinHook](https://github.com/TsudaKageyu/minhook), [Dear ImGui](https://github.com/ocornut/imgui) 1.91.9b, [nlohmann/json](https://github.com/nlohmann/json) - each under its own license |

## Known issues

- **Instant Dominate**: switch it off for quests that ask you to interrogate an orc - the quest script expects the
  normal domination time and can get stuck.
- **Raise the Dead**: read the warning in the trainer. Switching it off only takes Dark Talion away if the trainer gave
  it (a Dark Talion earned in the story is kept).
- Options are refused while the game is loading ("wait until you are in the game world") - try again once you are
  playing.
- If something goes wrong, the trainer writes `trainer.log` next to the DLL. Include it in a report.

## Credits

The developer-switch addresses for Focus / Might / Elf Shot / Wrath were first documented in SeiKur0's Cheat Engine
table.
