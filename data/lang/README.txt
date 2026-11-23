TRANSLATING THE TRAINER
=======================
1. Copy _template.json and name the copy after your language: de.json, fr.json, pt-BR.json, ru.json, ja.json ...
2. Open it in a text editor that saves UTF-8 (Notepad does: Save as > Encoding: UTF-8).
3. Set "_language" to the name of your language as it should appear in the trainer.
4. Each line is  "English text": "your translation",  - write only between the second pair of quotes.
   Leave a line empty ("") to keep it in English. Never change the English side.
5. {1} {2} are parts the trainer fills in (numbers, names). Keep them; you may move them.
6. Inside a translation write \" for a quote and \\ for a backslash.
7. In the trainer: Settings > Language. After editing, press "Reload languages" - no restart needed.

If your language does not show up in the list, the file has a typo (usually a missing comma or quote).
The line starting with [lang] in trainer.log says where.

"_font" is normally empty. If letters still show as ?, put a .ttf font in this folder and write its file name there.
Right-to-left and joined scripts (Arabic, Hebrew, Persian) and Thai marks are not laid out correctly yet.
Item, orc and effect names come from the game's data and stay English, and so does trainer.log.
The included languages are machine-made drafts, not checked by native speakers. Corrections are welcome on Nexus or
Discord.
