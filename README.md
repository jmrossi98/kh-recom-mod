# kh-recom-mod

Replaces the card combat in Kingdom Hearts Re:Chain of Memories with something
closer to Kingdom Hearts II. PC, Steam release of HD 1.5+2.5 ReMIX only.

Very much a work in progress.

## What works

- Attack always swings a Kingdom Key 9, whatever card is selected, and never
  uses the card up
- Magic and item cards play as themselves and stay in the deck
- No card breaks: the two sides' cards never interact, so nothing gets broken
  or refused in either direction
- Reloading is instant

## Layout

- `scripts/battle.lua` - LuaBackend script. Does the simple memory tweaks and
  loads the dll.
- `src/combat.cpp` - native side, hooks the game's play-card call.
- `re/` - tools I use for poking at the game: a memory scanner, a snapshot
  differ, a watcher for the card battle state, and Ghidra headless scripts
  (xrefs, decompile, disassemble).

## Building

Needs the Visual Studio 2022 build tools. Close the game first, the dll is
locked while it runs.

    build.bat

## Installing

1. Install [LuaBackend Hook](https://github.com/Sirius902/LuaBackend) into the
   game folder.
2. In `LuaBackend.toml`, point the `[recom]` scripts at this repo's `scripts`
   folder and use the Steam `game_docs` line.
3. Fix `DLL_PATH` at the top of `scripts/battle.lua` if the repo is not in the
   same place as mine.

F2 in game opens the LuaBackend console, F1 reloads the script.

## Notes

All addresses are for the Steam build. The script checks a few code bytes on
startup and turns itself off if they do not match.
