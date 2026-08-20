# Build prompt — `pgtools conflicts`/`patch` silently exclude BSA-packed mod files

Copy this whole file into a terminal Claude session rooted in this repo (or via `Start-Claude.ps1`,
which also loads the MSVC/vcpkg build environment). This is a real, confirmed bug affecting actual
patch output correctness, not a cosmetic issue — found live while testing the companion web tool at
`vortex-tools/vortex-collection-tools` (its own PGPatcher Load Order Editor).

## What's wrong, confirmed

`PGTools/src/main.cpp` calls `pgd.populateFileMap(false)` in **both** the `conflicts` subcommand
(line ~358) and the `patch` subcommand (line ~170) — the `false` means BSA-packed files are
excluded from the scan entirely, only loose files on disk are considered.

The real GUI executable (`PGPatcher/src/main.cpp:437`) calls `pgd->populateFileMap(true)` —
**includes** BSA-packed files.

No comment anywhere explains why PGTools diverges from the GUI on this point — it reads like an
oversight, not a deliberate choice.

## Real-world impact, confirmed live (2026-08-19)

Tested against a real ~2000-mod install. Multiple mods that show a real **PBR** shader match in the
actual PGPatcher GUI (checked, "PBR" in the Shader column — confirmed reproduced three times:
"Daggerfall Archmage Outfit Remake - 3BA PBR 2K 1.1", "Faultier's PBR Landscapes 4k", "Dwemer Armor
PBR Patch") come back with `"shaders": ["Default"]` — no PBR match at all — from a direct
`pgtools conflicts truepbr` call against the same install, same settings. TruePBR shader detection
(`PGDirectory.cpp:107-116`) fires when it finds a `pbrnifpatcher/*.json` config file alongside the
mesh — if that mod ships its PBR config packed inside a BSA rather than as loose files, `populateFileMap(false)`
means PGTools never even sees it.

This means **`pgtools patch`** (the actual, real output-generation pipeline our web tool's own
`/build` route calls) is ALSO silently skipping BSA-packed content compared to what the real GUI
would produce — this isn't just a display gap in the companion web tool, it's a correctness gap in
PGTools' own real patch output.

## The fix

Change `populateFileMap(false)` to `populateFileMap(true)` at **both** call sites in
`PGTools/src/main.cpp` (currently around lines 170 and 358 — confirm the exact current line numbers
before editing, this prompt's line numbers may have drifted). Match the real GUI's own behavior
exactly — that's the whole point, PGTools should produce the same real result the GUI does, just
from a script/CLI instead of clicking through a dialog.

**Before changing anything, re-verify these claims against the actual current source yourself** --
this prompt describes what was true when written, don't trust it blindly:
- Confirm `PGPatcher/src/main.cpp`'s own `populateFileMap` call is still `true`, and there isn't some
  OTHER reason (a different flag, a later call) that makes the GUI's real behavior more nuanced than
  a flat `true`.
- Confirm there isn't a reason PGTools deliberately excludes BSAs that isn't captured in this repo's
  own comments (check git blame/history on that line if it helps) -- e.g. a performance reason for
  the dry-run `conflicts` path specifically. If you find a real reason, flag it and don't just
  silently apply the fix; surface it back instead.
- Check whether `populateFileMap`'s own signature/behavior for `true` has any prerequisite (e.g.
  needing BSA files indexed/available at the given `--source` path) that could break the `conflicts`/
  `patch` subcommands in some other way once BSAs are included -- read the actual function body
  before assuming this is purely additive.

## Build & verify

Use `.\build.ps1` (already in this repo's root — a fast incremental rebuild of the existing
`build/` directory, not the full `buildRelease.ps1` release-packaging script) to rebuild `pgtools.exe`
after the change:
```
.\build.ps1
```

**Real verification, not just "it compiles":**
1. Run `pgtools conflicts truepbr --source <the real Skyrim Data folder> --output <a throwaway temp
   dir> --cfg-dir <the real PGPatcher cfg folder> --json-output <a throwaway json path>` against a
   real install with mods known to have BSA-packed PBR configs (the three named above are confirmed
   real examples on the director's own install, if you have access to test against it — otherwise
   confirm with the director which mods to check).
2. Confirm those specific mods now show `"shaders": ["Default", "PBR"]` (or similar, a real non-
   Default entry) instead of `["Default"]` alone.
3. Confirm mods that were ALREADY working correctly (loose-file PBR mods) still work identically —
   this change should only ADD detection for BSA-packed content, never change results for mods that
   were already being read correctly.
4. This is a real behavior change to the actual patch pipeline (`pgtools patch`), not just the
   dry-run `conflicts` path -- if you can safely test a real (or throwaway-output) `patch` run too,
   do so and confirm it doesn't error out or behave unexpectedly with BSA-inclusion now on.

## Handoff

Write the standard handoff to this repo's own `prompts/handoff-latest.md` when done (create the
`prompts/` folder's handoff file if this is the first task run against this repo — there's no
existing CLAUDE.md/queue.json here yet, this is genuinely the first real terminal task routed
through this repo). Include: what changed, the real before/after shader-detection result for at
least one of the three named mods, and whether you found any reason the exclusion might have been
deliberate that's worth the director knowing about before this ships.

COMMIT + PUSH when verification passes.
