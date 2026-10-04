# kh-recom-mod

Replaces the card combat in Kingdom Hearts Re:Chain of Memories with something
closer to Kingdom Hearts II. PC, Steam release of HD 1.5+2.5 ReMIX only.

Very much a work in progress.

## What works

- KH2 style command menu in battle: Attack, Magic, Items, Summon. Up/down
  (D-pad or arrow keys) move the cursor, the game's confirm button acts,
  B / Backspace / left backs out of a list
- Attack always swings a Kingdom Key 9 and never uses a card up
- Magic, Items and Summon open a list built from what is in the deck. Nothing
  leaves the deck when used
- No card breaks: the two sides' cards never interact, so nothing gets broken
  or refused in either direction
- The deck can no longer be scrolled or switched by the player, the menu owns
  those buttons
- Reloading is instant

## Not done yet

- The card stack is still drawn underneath the menu
- Magic and items are free, there is no MP
- Enemies still play cards
- Enemy cards and friend cards are unreachable (they live on the deck's second row)
- Menu art is placeholder shapes and a system font

## Layout

- `scripts/battle.lua` - LuaBackend script. Does the simple memory tweaks and
  loads the dll.
- `src/combat.cpp` - native side, hooks the game's play-card call and the card
  battle update.
- `src/overlay.cpp` - draws the command menu over the game. The game presents
  from a D3D12 swap chain, so this hooks Present and draws through D3D11On12.
- `tools_dev/` - small test programs that render the menu to a BMP without the
  game, one on a D3D11 swap chain and one on D3D12. Build with `build_test.bat`.
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
