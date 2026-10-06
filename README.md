# kh-recom-mod

Replaces the card combat in Kingdom Hearts Re:Chain of Memories with command
menu combat in the style of the first Kingdom Hearts. PC, Steam release of
HD 1.5+2.5 ReMIX only.

Very much a work in progress.

The game's own portrait and HP gauge stay, moved to the bottom right. For
KH2's gauge, portrait and command menu instead there is a second mod, which
works with this one or by itself:
[kh-recom-kh2-hud](https://github.com/jmrossi98/kh-recom-kh2-hud). Neither
needs the other and neither holds the other's code; when both are running
they find each other through a small block of shared memory.

## What works

- KH1's command menu, built from KH1's own menu pieces, header words and
  icons and laid out, coloured and animated by KH1's own numbers (its menu
  code and the sprite files it loads): Attack, Magic, Items, Summon. Blue
  outside battle and red in it, the selected slot lit and sliding out to the
  right, lists in their own colours opening over the menu. Up/down (D-pad or
  arrow keys) move the cursor, the game's confirm button acts, B / Backspace /
  left backs out of a list. Shown outside battle too, where it is only for
  looks. Labels are in KH2's font for now
- The context command over the menu when something is in reach outside
  battle: Open at a door or chest, Save on a save point, Talk, Lift, Warp.
  Display only, the game's own button still does it
- The game's own menu sounds: cursor, open, back, confirm, error
- Donald and Goofy head the Summon list and are always there
- Hidden while the game's start menu, pause screen or save menu is up
- The game's portrait and HP gauge sit bottom right instead of top left, and
  the map card panel and the row of friend cards are out of the way
- With the KH2 HUD mod running this mod draws no menu: it passes the menu's
  state across and that mod draws it in KH2's style
- Attack always swings a Kingdom Key 9 and never uses a card up
- Magic, Items and Summon open a list built from what is in the deck. Nothing
  leaves the deck when used
- Magic is free. With the KH2 HUD mod installed there is MP: spells cost MP,
  Cure takes all of it, and at zero it recharges with magic locked until it
  is full
- No card breaks: the two sides' cards never interact
- The card HUD is hidden: the deck and the cards played by both sides. F6
  brings it back
- The deck can no longer be scrolled or switched by the player

## Not done yet

- Combat itself is not KH1's yet: Sora's combos, timing and the enemies are
  still the card game's underneath
- Enemies still play cards (you just can't see them)
- Calling Donald or Goofy still dims the battlefield the way a friend card does
- Enemy cards are unreachable (they live on the deck's second row)
- Items and summons are free

## Art

No game art is in this repo. `tools_dev/make_assets.py` builds what the overlay
needs from your own extracted copies of Re:CoM, KH2 and KH1 (all in HD
1.5+2.5 ReMIX) into `build/assets`. Without those files the overlay falls
back to plain shapes.

    python tools_dev/make_assets.py <extracted recom> build/assets <extracted kh2> <openkh Apps folder> <extracted kh1>

## Layout

- `scripts/combat.lua` - LuaBackend script. Does the simple memory tweaks and
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
3. Fix `DLL_PATH` at the top of `scripts/combat.lua` if the repo is not in the
   same place as mine.

F2 in game opens the LuaBackend console, F1 reloads the script.

## Notes

All addresses are for the Steam build. The script checks a few code bytes on
startup and turns itself off if they do not match.

For the first three minutes after launch the dll watches for the game hanging:
if no frame comes for 20 seconds it writes how far it got to
`build/recom_combat.dll.log` and ends the game, since a hang in exclusive
fullscreen otherwise takes the desktop with it.
