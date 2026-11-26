SHADOW OF WAR TRAINER
=====================

An in-game trainer for Middle-earth: Shadow of War (single player).

REQUIREMENTS
- Shadow of War on Steam, game version 1.0.8874 (the current Steam build).
  On any other version the trainer still opens but switches every feature off by itself.
- Windows 10 or 11, 64-bit.

HOW TO USE
1. Copy xinput9_1_0.dll and this ShadowOfWarTrainer folder into the game's x64 folder (next to ShadowOfWar.exe).
   INSTALL.txt has the steps.
2. Start the game from Steam as usual. The game loads the trainer by itself.
3. In game, press INSERT to open or close the trainer (mouse, keyboard or gamepad).

Your choices are remembered between sessions. Options that were on are switched on again once you are back in
the game world (not during loading screens).

BEFORE YOU START
- Back up your saves. Gear, level, skill and item edits are saved into your game.
  Steam saves: C:\Program Files (x86)\Steam\userdata\<your id>\356190\remote
- Single player only. Do not use it in online modes.

WHAT IT CAN DO
Player     Infinite Health, No Summon Cooldown, Heal Now, Infinite Focus / Might / Elf Shot / Elven Rage,
           Might Never Drains, Elven Rage Refills After Use,
           Wraith Chain: chain without aiming, no slow-motion, fast chain.
Skills     Level, Skill Points, Mirian, Give Item, Choose Multiple Skills, Unlock All Skills in the Menu,
           Activate All Upgrades, Raise the Dead (Dark Talion), Prestige for every gear slot.
Gear       Edit any item: level, effects (add / replace / remove, values), look (transmog with preview),
           rarity / type, Auto-Complete Gear Challenges.
Inventory  Copy or remove gear, add any captain gear, add an edited item.
Loot       Choose what captains drop and how often, or make them drop an edited item.
Army       Edit followers: level, intel, bodyguard, upgrades, gear, role, name, rarity.
           Instant Dominate (the domination gauge fills at once), Dominate Captains at Full Health,
           recruit any orc, create new orcs.
World      Infinite Mission Time.

LANGUAGES
Settings > Language. The translations are machine-made drafts: corrections are welcome. To change one or add your
own language, see lang\README.txt.

KNOWN ISSUES / TIPS
- Instant Dominate: switch it OFF for story or side quests that ask you to interrogate an orc. The quest
  script expects the normal domination time and can get stuck.
- Raise the Dead: read the warning in the trainer. If it causes trouble, switch it off; the trainer only takes
  Dark Talion away again if the trainer gave it (a Dark Talion earned in the story is kept).
- Options are refused while the game is loading ("wait until you are in the game world"): try again once
  you are playing.
- If something goes wrong, the trainer writes trainer.log next to ShadowOfWarTrainer.dll. Include it in a report.
  Older sessions are kept in the logs folder (the last 20).
- If the game crashes, the trainer saves that session's log in logs\crashes (crashes_001.log, ...), where it is
  never overwritten or deleted (20 crashes per file, then a new file starts, so older crashes stay there for
  tracing). You will see a message about it the next time you start the game. Send the newest
  crashes_###.log when you report a crash. Settings > Crash logs opens the folder.

REMOVING IT
Close the game, then delete xinput9_1_0.dll and the ShadowOfWarTrainer folder from the x64 folder. Nothing else in the
game folder is changed. To play without it for a while, rename xinput9_1_0.dll to xinput9_1_0.dll.off.
