# Save write failure audit

A forced short-write test (POSIX RLIMIT_FSIZE=64) reproduced a truncated staged
file being reported successful by SDL_SaveFile on the installed SDL build.
The staged file contained 64 bytes instead of 131,072, and the old save would
have been replaced. CR1/Omega's previous .part+SDL_SaveFile+rename sequence was
therefore also affected, despite already staging its writes.

All four ports now use `platform/atomic_file.hpp`. Each write owns an exclusive
staging directory on the destination filesystem, writes and explicitly checks
flush and close, then replaces the destination only on success. Stages are
cleaned after success and failure. Failed staging/replacement preserves the old
save. This protects against failed writes and process interruption, and does
not claim filesystem/power-loss durability.

CR2/CR3 progress and system saves, and clearing continue data, use this helper.
CR3's Copy File path also uses it; failed copy/delete operations now show a
notice instead of reporting completion. Legacy save confirmations show an error
if saving failed. CR3 native autosaves remain pending after failure and retry
at five-second intervals; a successful save clears the pending/retry state.

`atomic-file` CTest covers creation, replacement, a UTF-8 destination, intentional
empty files, missing parents, failed rename, forced short write, old contents
preserved on failure, and stage cleanup. The real CR3 storage check enters its
prologue, makes the selected destination temporarily unwritable as a directory,
verifies failure preserves it and schedules a retry, then removes that fixture
and verifies a successful, loadable save clears the retry. Only isolated test
profiles are used.

All 46 CTests pass in native and GLES ASan/UBSan builds (CR1 7, Omega 7, CR2 15,
CR3 17). Native CR3 failure/recovery checks pass with Android, iOS and Japanese
Vita data in both builds (six runs). LeakSanitizer is disabled for the sandbox
tracing limitation. No player saves were modified. Changes are local and
unpublished; physical-device behavior remains to be checked.

## Follow-up: saves cut short (2026-10-30)

Atomic replacement stops this port writing half a save, but a save can still
arrive cut short from another tool, a copied file or a damaged disk. Those files
used to be accepted: CR2/CR3 took a header alone as a whole game, and CR1/Omega
took anything after the `map` line, which loads as a game with no party or flags.

The rule has to hold for every layout that exists, so it looks only at what they
all share. The file must end in a newline, and:

- CR1/Omega: `flags`, `vars`, `items`, `oz`, `noencounters`, `walkexp` and a full
  party. `boxes` must not be required: the Vita import (`vita_import.hpp`) writes
  this text itself, with the party last and no `boxes`, `bands` or `treasure`.
- CR2: the closing `map` key. CR3: `area`, or `map` for older saves and
  `cr3tool save-import`. Older CR2 saves lack several newer keys, so those keys
  are not required.

CR2/CR3 now parse into a copy, so a refused file leaves the game unchanged.
CR2's title stays open on a refused file, says "Could not read this save. The
file has been kept." and returns to its buttons on OK. It used to quit.

Limit: in CR1/Omega a cut exactly between the `walkexp` and `boxes` lines, or
among the lines after them, looks like an older save, and loads without those
later lines.

Tests: `save-integrity` (CR2/CR3) cuts a save at every byte before the closing key
(1069 and 1564 cuts). The CR1/Omega session check does the same in both layouts.
Real files were read back without writing to them: all seven CR1 profile saves,
including one from before `bands` and `treasure` existed; five CR1 and three
Omega saves through the real Vita import; and the CR2/CR3 saves, including a
pre-release CR2 save.

## Follow-up: damaged numbers in a whole save (2026-10-30)

A save can be complete and still hold impossible numbers. Each port now holds them
to limits set at load, or refuses the save if it names a place no game has.

- CR1/Omega: an out-of-range `area` read past the area table and crashed. `area`
  and `map` are now checked when the save is read. Party numbers are held in range.
- CR2/CR3: a flag number grew the flag table to fit it, and each autosave then
  walked the whole table: under the sanitizers, a hang. Flags from 65536 up are
  ignored. Difficulty, the selected dungeon and level, a character's level,
  experience and MP, and sopia experience are held in range. The experience
  percentages use 64-bit sums. `system.txt` keeps only flags below 992, the only
  ones it is ever written with.

Limits are checked against the games' own tables, not only against saves seen so
far: level 200 in CR2 needs 39703470 experience, far more than any save here, and
a first limit of 10 million would have cut late-game saves. `save-integrity` now
checks that a level 200 character keeps its numbers. Item and map names in a save
are not limited this way; a bad one is not found and the load fails cleanly.

Tests: seeded fuzzing under ASan/UBSan, every field of real saves with hostile
values plus random damage: CR1 486, Omega 406, CR2 526 and CR3 1186 cases, reaching
the Camp pages, and in CR1/Omega a battle (`CR_SELFTEST_SAVE_DEEP=1`). The four
ports' `settings.ini` and `system.txt`: 460 cases. The Vita importer with
checksums recomputed: 70000 cases plus JSON nesting to 2 million. After the
fixes all of these run with no sanitizer report.

A later pass ran damaged CR2/CR3 saves through battles and CR3's native menus, the
gene lab, the Space Port and VS mode: 3428 cases sampled across every save key,
under the sanitizers. It found two number overflows in CR3 and no crash or hang.

Limits: CR3's ordinary shops, world select and pub were not reached, and each save
key was sampled, not tried in full.
