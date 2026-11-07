# Cadaver (native) - work in progress

Hand-written C game core (`src/game.c`, no OS calls) + Win32 front end (`src/win_main.c`).
Game data is NOT included. Extract it from your own Cadaver disks:

    python3 tools/export_level.py "Cadaver Disk 2 (Levels).adf" game_image.bin 1 data/level1.bin

(needs Python 3, Pillow, numpy; `game_image.bin` = the main game image from Disk 1, see the project notes.)
Build: `cmake -S . -B build && cmake --build build --config Release`, run `cadaver.exe` next to `data\level1.bin`.
Controls: arrows / WASD. Esc quits. Ctrl or Space = fire (menu, jump). F1 or M = map of the rooms entered. Re-run the export after updating: creature animation and movement data were added to the level files on 2026-11-07 (older files still load, but creatures stay still). F6-F10 load data\level1.bin ... level5.bin (export each with the last argument of the tool = level number). `dump` is a headless test driver: `dump data/level1.bin "R45,U12" out`.

Status: level 1 rooms, walking, pushing, room exits. No HUD, items, enemies, sound or doors yet.
