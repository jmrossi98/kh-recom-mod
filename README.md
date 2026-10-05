# kh-recom-mod

Replaces the card combat in Kingdom Hearts Re:Chain of Memories with something
closer to Kingdom Hearts II. PC, Steam release of HD 1.5+2.5 ReMIX only.

Very much a work in progress.

## What works

- KH2 command menu, drawn from KH2's own sheet, layout data and font
  (Twilight Town's skin for now): Attack, Magic, Items, Summon. Up/down (D-pad
  or arrow keys) move the cursor, the game's confirm button acts, B / Backspace
  / left backs out of a list. Shown outside battle too, where it is only for looks
- KH2 status gauges bottom right: KH2's portrait of Sora in his KH1 clothes
  (normal, hurt, down), the HP ring growing clockwise with max HP and spilling
  into a bar once it is full, and the MP bar
- Attack always swings a Kingdom Key 9 and never uses a card up
- Magic, Items and Summon open a list built from what is in the deck. Nothing
  leaves the deck when used
- MP: spells cost MP, Cure takes all of it, and at zero it recharges with magic
  locked until it is full
- No card breaks: the two sides' cards never interact
- The game's own HUD is hidden: deck, played cards on both sides, portrait,
  map card panel. F6 brings most of it back
- The deck can no longer be scrolled or switched by the player

## Not done yet

- Enemies still play cards (you just can't see them)
- Party members: F5 / F7 call Donald / Goofy without a friend card as a test,
  nothing automatic yet
- Enemy cards are unreachable (they live on the deck's second row)
- Only the Twilight Town menu skin; no per-world skins yet
- Menu has no opening animation and the header's smoke is left out
- Items and summons are free

## Art

No game art is in this repo. `tools_dev/make_assets.py` builds what the overlay
needs from your own extracted copies of Re:CoM and KH2 (both are in HD
1.5+2.5 ReMIX) into `build/assets`. Without those files the overlay falls
back to plain shapes.

    python tools_dev/make_assets.py <extracted recom> build/assets <extracted kh2> <openkh Apps folder>

## Layout

- `scripts/battle.lua` - LuaBackend script. Does the simple memory tweaks and
  loads the dll.
- `src/combat.cpp` - native side, hooks the game's play-card call and the card
  battle update.
- `src/overlay.cpp` - draws the command menu over the game. The game presents
  from a D3D12 swap chain, so this hooks Present and draws through D3D11On12.
- `tools_dev/` - `make_assets.py` (see Art), two test programs that render the
  overlay to a BMP without the game (D3D11 and D3D12 swap chains, build with
  `build_test.bat`), and `kh2seq.py`, a software renderer for KH2 sequence
  files used to work out what is in a .2dd.
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
