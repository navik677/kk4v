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

- `FlowTrackerPlugin.ks`: `@eval exp="var skbg=...'+f.route"` then
  `@bg cond="...!=skbg" file=&skbg ...`
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

The fix here is content-only: replace the bare `var skbg`/`var skip`
with `f.skbg`/`f.skip` (`f` is the game's own persistent scenario-flag
object, already used this way everywhere else in these same scripts),
so the value survives on `f` for the very next tag to read. All other
lines are untouched -- verified by diffing against the original
decrypted archive content.

Symptoms before the patch: background never renders during a specific
skip-BG check (silent, one-time), and the entire TYPE-MOON opening logo
animation is skipped outright (every `cond=!skip` tag throws and is
treated as false).
