# Fate/stay night [Realta Nua] loose-file script patches

These are drop-in replacements for two scenario scripts that ship inside
the game's encrypted XP3 archives (`patch2.xp3` / `RealtaNua.xp3`, both
byte-identical for these two files). Kirikiri checks for a loose file on
disk before falling back to the archive, so copying these into the game
folder overrides the archived version without touching any `.xp3`.

Install: copy both files into `ux0:data/kirikiroid2/fate-stay_night/`
(same folder as `Fate.exe`/`data.xp3`), exact filenames as-is (UTF-16LE
with BOM, matching the engine's expected TJS/KAG script encoding).

## Why

Both scripts declare a scenario-local variable with `var` inside one
`@eval exp="..."` tag, then reference it from a *separate* tag's
`cond=`/`file=&` attribute right after:

- `FlowTrackerPlugin.ks`: four occurrences --
  `var skbg=...'+f.route` then `@bg cond="...!=skbg" file=&skbg ...`;
  `var _date=...`/`var _title=...` then `[emb exp=_date]`/`[emb exp=_title]`
  (the day-title card, e.g. "1st Day: Breakfast preparation..."); and
  `var flow_tracker_flag=...` then `@jump cond="flow_tracker_flag==1"`
  and `@if exp="flow_tracker_flag==2"` right after -- this last one is
  what actually caused the black screen on pressing START, since it
  gates whether the next scene's script even runs.
- `ロゴ.ks` (the TYPE-MOON opening logo): `@eval exp="var skip=false"`
  then `cond=!skip` on ~14 separate `@move`/`@waittrig`/`@trans` tags

Each `@eval`/`cond`/`file=&` attribute is evaluated as its own, separate
top-level script execution. A `var` declared in one such execution does
not survive into another, even when both share the same context object
-- confirmed empirically against this port's TJS engine (see the KK4V
git history around commits 3db7152..14702db for the full investigation,
including two failed attempts at an engine-level fix). No official fix
for this exists in any of the game's own patch archives (`patch.xp3`,
`rufix.xp3`, `patch5.xp3` were checked).

The fix here is content-only: qualify each bare name (`skbg`, `_date`,
`_title`, `flow_tracker_flag`, `skip`) onto `f.` (the game's own
persistent scenario-flag object, already used this way everywhere else
in these same scripts), so the value survives on `f` for a later tag
to read. All other lines are untouched -- verified by diffing against
the original decrypted archive content.

Symptoms before the patch: background never renders during a specific
skip-BG check (silent, one-time), and the entire TYPE-MOON opening logo
animation is skipped outright (every `cond=!skip` tag throws and is
treated as false).

## afterinit.tjs -- missing global Stretch()

Once the two scripts above were fixed, the logo intro plays fully and
crashes at its end: `DashPlugin.ks`'s `finish()`/`splineMoving()` call a
bare global `Stretch(%[src:...,dest:...,...])`, which throws `Member
"Stretch" does not exist`. This isn't a script bug -- scanned all ~850
`.ks`/`.tjs` files across every archive (`data.xp3`, `patch2.xp3`,
`rupatch.xp3`, `RealtaNua.xp3`, `rufix.xp3`) and none define `Stretch`.
On the real Windows engine it comes from `util.dll` (present in the
game folder, a common third-party kirikiri plugin), loaded natively
before any scenario script runs. This port's `TVPLoadPlugin` is
stubbed out (can't run native x86 DLLs on Vita/ARM), so the global
was simply never defined here.

`afterinit.tjs` is a *built-in engine hook* (checked by the stock
kirikiroid2/krkrz engine itself before the first scenario script runs,
independent of anything KK4V-specific) -- copying it into the game
folder is enough for the engine to load and run it automatically. It
defines `global.Stretch` as a thin wrapper around the layer's own
native `operateStretch()` (which this engine does implement), matching
the parameter dictionary shape (`src`/`sleft`/`stop`/`swidth`/`sheight`
+ `dest`/`dleft`/`dtop`/`dwidth`/`dheight` + `opa` or `opacity`) that
`DashPlugin.ks` and similar effect scripts call it with.

Install: copy `afterinit.tjs` into the same
`ux0:data/kirikiroid2/fate-stay_night/` folder alongside the two files
above.

## Full sweep for the same cross-tag var pattern

After finding the same bug three separate times (skbg, then
_date/_title/flow_tracker_flag, all in FlowTrackerPlugin.ks), scanned
every `.ks` file in `RealtaNua.xp3` (~780 files) for the same shape:
a standalone `@eval exp="var NAME=..."` tag whose NAME is referenced
again elsewhere in the file. 15 files matched; one (`voice.ks`, `_b`)
turned out to be a false positive -- the only reference is commented
out (`;//...`) so it never actually runs. The rest are real and are
included here, same fix (qualify onto `f.`), each diffed against the
original to confirm no unrelated line changed:

- `タイトル.ks` (the actual title-screen script, run right after the
  logo/prologue sequence) -- `es`/`skip`. This is very likely the
  direct cause of black boxes on the title screen and the logo
  appearing to "hang": `@if exp=!skip||...` throws and is treated as
  false, so the caution-screen block is silently skipped, and a
  separate uncaught throw on `sf.effectSkip=es` triggers an error
  MessageBox that stalls everything until dismissed.
- `ConditionPlugin.ks` -- `tmp`/`tmpcnt` (a repeated nega/change_condition
  flicker effect, used by "bad ending" style condition reveals).
- `マクロ.ks` -- `___scrsize`/`___curfullscreen` (fullscreen toggle
  macro), `__font` (message-font-change macro), `changed`
  (`@changefg` character-swap macro).
- `DashPlugin.ks` -- `tx`/`ty`/`src`, used only in one specific
  `@eval`/`@eval` pair (line ~741-743, a foreground/background swap
  effect). Patched by exact line match, not a whole-file substitution:
  `tx`/`ty`/`src` are also genuine local variables inside unrelated
  functions elsewhere in this file, so a blind find/replace would have
  broken those.
- `セイバーエピローグ.ks`, `凛エピローグ.ks`, `凛エピローグ2.ks` (twice),
  `桜エピローグ.ks`, `桜エピローグ2.ks`, `ミニ劇場その1/2/3.ks` -- all the
  identical `es`/`sf.effectSkip` pattern from `タイトル.ks`, copy-pasted
  across every epilogue and mini-theater route.

Install: copy each of these files into
`ux0:data/kirikiroid2/fate-stay_night/` the same way.
