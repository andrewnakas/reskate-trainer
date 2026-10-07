# ReSkate Trainer

The PHYSICS page in the ReSkate menu (Insert): live physics tuning, tricklining, Skate 3 presets, practice markers and a
telemetry HUD. It ships no game data: the list of values is built at run time from the player's own
`Gameplay/SkatePhysicsTuning`.

## What it does

The PHYSICS page opens on **Feel**, with five reference points: **Hardcore, Authentic, Stock,
Accessible and Arcade**. Choose a reference or move the mixer to preview supported controls;
**Apply this feel** commits the result. The preview respects both direct locks and locks inherited
from linked controls. Unrelated edits stay as they are. Stock in the mixer restores only the
curated controls; Settings > Reset and maintenance contains the broader resets.

- **Feel:** full-width stock-relative sliders, grouped into pop and airtime, speed and rotations,
  landings and bails, grinds and slides, **Flips and catches**, and **Transition pumping**. The control label stays above its editor. Drag to
  adjust; Ctrl-click or double-click to type; Enter commits and Escape cancels. Edits commit
  when editing finishes rather than sending a command every frame.
- **Settings:** search the complete value table, select a group, show changed or locked controls,
  and optionally reveal unverified values and individual graph points. Labels have tooltips for
  their complete name and description. Arbitrary values use full-width numeric drag editors.
- **Presets:** save physics or trick-only setups, review saved inputs before applying, import and
  share presets, or select a preset to apply when a map loads. Replacing and deleting saved
  presets require a confirmation. Imports add to the library without applying and use a new
  name when one is already taken. The existing Skate 3 reference presets remain available here.
- **Fun:** Tricklining and reverts opens first, with precise momentum editors and collapsible advanced tuning. Jump heights follows; quick toggles sit below in six collapsible categories, with visible descriptions, ON/OFF states and active counts. Never bail stays in Skater > Movement.
- **Tools:** Practice and Map & HUD. Practice has game speed, five marker slots per map,
  bail returns and teleport. Map & HUD has telemetry, CSV recording and map recommendations.
  The unreliable trainer camera controls were retired upstream in 0.4.2.

**Hardcore grinds:** rail lock-on, boardslide capture and nose/tail-slide capture use **0.1x this
map's stock distance**. Common and curb friction
use 1.4x stock, and grind pop heights use 0.75x stock. Entry speed and angle requirements are
independent and stay unchanged by these rules. These profiles are starting points for playtesting;
they are not calibrated simulations of real skating.

**Existing trainer controls:** this workshop keeps upstream 0.4.3's flip, catch and pump controls.
Feel > Flips and catches holds the global board-flip speed, independent speeds for all sixteen
supported tricks and automatic catch timing. Speeds combine multiplicatively; nollie uses its
regular trick. Slow flips automatically keep the game from rushing the board before landing.
The old slow-flip switch is retired; old saved keys still import. Timed catch takes priority
over custom speeds without deleting them. Its estimate assumes level ground; gaps and drops
can catch earlier, and 100% can land still flipping. It is assistance, not manual foot control.

Feel > Transition pumping holds the real Pump power option: 1 is stock, above 1 adds speed
while the game detects pumping, below 1 removes speed. Animation and pump discovery can take
about 15 seconds after a map load; the controls show the pending state. Labels sit above
full-width sliders. Ctrl-click types precisely; Enter applies and Escape cancels.

Fun > Jump heights contains the four jump heights. Board bending appears once, under Tricklining and reverts.
Detailed revert rules stay under Tricklining and reverts. Ordinary 180s no longer earn bending
speed by default: upstream migrates the old default off-travel threshold of 12 degrees to off
(90 degrees). Custom preset values remain stored. Reset tricks now confirms that it also
resets the flip, catch and pump options in Feel, including the default 70% catch point;
saved presets remain in the library.

Saved presets compose: applying one changes only the values it contains. Unlisted options
keep their current settings. Review the saved values and reset trick options first when you
want a clean comparison. A named library preset selected for a map can include trick options;
a preset embedded by the map author currently applies only the tuning-value table.

The HUD, jump read-out and marker shortcuts are off until enabled in Tools. The controller menu
shortcut is **LB + RB + click the right stick**; it can be disabled in Practice. D-pad or left
stick navigates, A presses and B goes back. With marker shortcuts enabled, hold **LB + RB**:
D-pad up saves, down returns, and left/right picks a slot.

Console commands (`~`) include:

`trainer open [feel|settings|presets|fun|practice|map]`, `trainer status`,
`trainer workshop <hardcore|authentic|stock|accessible|arcade|-1..1>`,
`trainer assist <0.1..5>`, `trainer set <id> <value>`, `trainer find <words>`,
`trainer preset apply|remove|save|delete|export <name>`, `trainer preset import [file]`,
`trainer dial <multiplier> <preset name>`,
`trainer reset <id>|all|tricks|presets|everything`, `trainer marker save|go|clear [slot]`,
`trainer option flip_speed <0.01..100>`, `trainer option flip_advanced <0|1>`,
`trainer option flip.kickflip <0.01..100>` (and the other fifteen trick keys),
`trainer option catch_at <0|1>`, `trainer option catch_percent <1..100>`,
`trainer option pump_power <0..1000>`,
`trainer tp <x> <y> <z>`, `trainer where`, `trainer jumps`, `trainer dump`, `trainer selftest`.

Legacy `trainer open` names still route to their equivalent section. The community trainer's
`trainer feel stock|easy|normal|hardcore` keeps its Skate 3 behavior; `trainer workshop` is the
separate stock-relative mixer.

Shared imports are limited to 2 MiB and 8192 finite numeric values. Invalid or oversized presets
are rejected as a whole, not truncated. Unknown keys are kept for other game builds. The profile
writer uses an adjacent temporary file and an atomic replacement, keeping the previous profile
if writing or replacing fails.

## For map makers: `trainer.json`

Put a `trainer.json` in your mod folder (beside `manifest.json`). Stock ReSkate ignores it; with the
trainer, players get your spots and your recommended tuning in Tools > Map & HUD.

```json
{
  "schema": 1,
  "note": "Built for about 60 km/h off the first lip.",
  "preset": {
    "name": "Gravy Train",
    "values": { "PhysicsPush.MaxPushableSpeed": 12.0 }
  },
  "spots": [
    { "name": "Start deck", "position": [0.0, 790.0, 0.0] },
    { "name": "Jump 3", "position": [0.0, 700.0, 310.0] }
  ]
}
```

`"level"` (a level asset) is optional: without it the file stands for every level the mod's
`reskate-levels.json` adds. Value ids are the ones `trainer dump` lists.

## Multiplayer

The trainer goes through ReSkate's own session rules instead of around them:

- While a session's host sets everyone's physics (the session's "enforce tuning", on by default),
  a guest cannot edit anything here; the page says so. They receive the host's shared setup: its
  tuning through ReSkate's host-tuning sync, and its class values, trick multipliers and auto push
  through the session's physics extras (`trainer_session.h`), sent whenever the host changes one
  and to players who join later. On a dedicated server all of it is the game's own.
- Advanced per-trick speeds, automatic catch timing, pump power and extended-push runtime
  assistance are not included in upstream SessionExtras. These upstream effects remain local; do not
  treat them as fully synchronized host/guest settings. The workshop displays enforced-host
  and boosts-disabled restrictions above every tab and keeps personal settings for later.
- With that switched off, everyone's physics are their own, and the trick multipliers and auto push
  follow the host's boosts permission like ReSkate's other boosts.
- Teleports and markers follow the host's noclip / teleport permission.
- Game speed is ReSkate's `SimulationTime.TimeScale` setting, which ReSkate locks in a session.
- Servers' `score_check` / `enforce_tuning` see a player's tuning changes like any other tuning mod.

It unlocks no cosmetics or entitlements.

## How it works

- `physics_tuning_model` keeps the field names the game's EBX carries, so `trainer.cpp` can build its
  table from `read_game_tuning()`.
- Edits go into a target copy of the asset; `physics_tuning::write_live()` writes the differences and
  refreshes the local skater's cached block. Values are re-applied after a level load.
- The game thread owns all state. The menu only queues `trainer ...` console commands and reads two
  snapshots (`trainer_view.cpp`).
- Jumps are measured from the skater's position each client tick: the game's own physics state says when the skater is in the air.
  Wipeouts come from the same state, which the no-bail hook already sees.
- Settings, presets, markers: `%LOCALAPPDATA%\ReSkate\trainer\trainer.json`.

## Checking a build

Enable the trainer regressions and point the tuning-name test at your supported game install:

```
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_TRAINER_TESTS=ON -DDINGOSDK_TEST_GAME_ROOT="C:/path/to/skate"
cmake --build --preset release --parallel 4
ctest --test-dir build/vs2022-x64 -C Release -R "^trainer_" --output-on-failure
```

On Windows these include actual ImGui interaction/layout, Feel rules, physics ownership,
profile replacement and preset import checks. `dingosdk_trainer_page_tests <output-folder>`
also writes native UI captures; it does not test game hooks or controller mapping.

```
RESKATE_STARTUP_COMMANDS="load <level asset>;wait 25;trainer selftest"
ReSkateLauncher.exe --no-gui --no-update
```

then read the `trainer selftest:` lines in `logs\ReSkate.log`. `dingosdk_trainer_tuning_dump <Skate
folder>` (CMake option `DINGOSDK_BUILD_TRAINER_TESTS`) lists the tuning values without the game.

## Known limits

- The jump read-out uses real time, so it reads low while game speed is not 1x.
- Masses and collision sizes (deck, trucks, wheels) only change on the next respawn.
- Curve and graph multipliers scale outputs only; a curve whose point count a mod changed keeps the
  mod's points.
- Not every tuning value is used by the game: about 4 in 10 rows have no code reading them
  (`trainer_used.inc`, found by a static pass over the game's code for this build). Ollie height
  comes from the `PhysicsJump` height graphs (the `PhysicsMode` jump heights are never read, so the
  trainer links them to those graphs: `value_links` in `trainer_presets.cpp`), body
  flips from `PhysicsReckoning.FlipScalar` and `FlipMaxSpeed` (`PerfectBodyFlips` forces exactly one
  rotation and ignores them), body spins from the `PhysicsBodyspin` graphs. "No use found" is not
  proof: the pass can miss a use.
- No comply, boneless and hippy jump heights are not tuning values: the game's trick scripts launch
  those. The trainer multiplies the launch speed where the game sets the jump's trajectory (no
  comply, boneless: `trainer_jump.cpp`) or scales the skater's upward speed as the jump starts
  (hippy jump).
- Much of the game's tuning is not in `Gameplay/SkatePhysicsTuning` but in data-defined classes
  (push speeds, everything on foot, dive and glide, bail speeds...). A live copy has no name to
  look up, so the trainer finds it by searching writable memory for the class's defaults laid
  out as the class lays them out (`trainer_classes.cpp`; the table in `trainer_classes.inc` is
  generated from the game's own data). The search reads all of the game's writable memory, so it
  only runs for a player with a use for it: one of those values or the flip speed is not the
  game's own (theirs, or a host's they skate with), or the Settings list is open. Then it runs
  on its own thread a few seconds after a level loads, and at most three times per level;
  `trainer classes` says what it found. Native code keeps its own copy of the push speeds, found
  and written the same way.
- "Push speed" scales the push class's speeds (a tapped push, a held one, the top) and the
  tuning's top pushing speed, which only gates whether a push may start. Auto push is the trainer's doing as well (the game's flag only reaches its animation): once rolling and not braking, the skater gains speed up to the auto push speed. Push strength has no effect in
  this game build and is hidden.
- Built for one game build (the one ReSkate 1.0.3 supports). A game update needs a new build.
