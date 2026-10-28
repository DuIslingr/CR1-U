# Importing Vita saves

Use the game's native **Options** screen from the title screen:

| Game | Option |
| --- | --- |
| CR1 / Omega | Game → Import Vita Saves → Choose Folder |
| CR2 | Import Vita Saves (below the volume bars) |
| CR3 | Other → Vita Saves → Import |

Choose a **decrypted, exported** Vita save folder: either the game's `PCSG` folder
or the parent folder containing all four games. The game validates the saves and
asks for confirmation with its own Yes/No controls. No is selected initially.
Confirm to import, then acknowledge the result and load a slot from the title.
No Python installation or separate tool is needed.

Existing files that the import replaces are copied to a new `vita-backup-...`
folder in the game's profile. Keep this folder to undo the import. Canceling or
rejecting an invalid manual save leaves the current saves untouched; an invalid
optional suspend save is reported and skipped. Source files are never changed.
Import is available from title-screen Options; the entry is disabled during play
so the current session cannot overwrite an imported save.
Game dumps and encrypted system backups are not save exports.

## Optional command-line converter

`tools/import_vita_save.py` converts **decrypted, exported** Vita saves for CR1,
Omega, CR2 and CR3 to the ports' own save formats. It requires Python 3.9 or newer,
with no extra packages. Game dumps and encrypted system backups are not save exports.

Close the game first. Choose either the game's `PCSG` folder or the parent folder
containing all four games. Give the destination explicitly:

```sh
python3 tools/import_vita_save.py /path/to/vita-saves --game cr1 --output /path/to/imported-cr1
```

Use `--game omega`, `--game cr2` or `--game cr3` for the other games. `--dry-run`
validates the source and lists the output files without writing anything.
The converter never modifies its input files. Existing output files are refused
unless `--replace` is supplied; replacements are backed up in a new
`vita-backup-<date>-...` folder inside the destination. Keep that backup to undo
an import. A failed conversion writes no saves.

The output folder can be a new staging folder, or the port's existing profile folder.
To play staged saves, copy the generated files into the relevant profile folder
while the game is closed. Do not copy them into the game asset folder.
The usual profile locations are:

| Game | Linux, below `$XDG_DATA_HOME` or `~/.local/share` | Windows, below `%APPDATA%` | macOS, below `~/Library/Application Support` |
| --- | --- | --- | --- |
| CR1 | `preservation/ChaosRingsHD/` | `preservation/ChaosRingsHD/` | `preservation/ChaosRingsHD/` |
| Omega | `preservation/ChaosRingsOmegaHD/` | `preservation/ChaosRingsOmegaHD/` | `preservation/ChaosRingsOmegaHD/` |
| CR2 | `DuIslingr/CR2-U/` | `DuIslingr/CR2-U/` | `DuIslingr/CR2-U/` |
| CR3 | `DuIslingr/CR3-U/` | `DuIslingr/CR3-U/` | `DuIslingr/CR3-U/` |

The command-line converter also writes `vita-import.json` with source hashes, output filenames and
any skipped suspend save. A bad manual save is rejected. A bad suspend save is
reported and skipped, and resume uses the valid manual save instead.

## What is imported

| Game / Vita title | Input | Output |
| --- | --- | --- |
| CR1 / `PCSG00497` | `SAVEDATA0001.bin`, optional `0002` suspend | `save1.dat`…`save4.dat` for routes with saved positions, and `resume.dat` |
| Omega / `PCSG00499` | `SAVEDATA0011.bin`, optional `0012` suspend | `save1.dat`, `save2.dat` for scenarios with saved positions, and `resume.dat` |
| CR2 / `PCSG00498` | `SAVEDATA0020.bin` system, `0021` play, optional `0022` suspend | `system.txt`, `save1.txt`, `cont.txt` |
| CR3 / `PCSG00457` | `SAVEDATA0030.bin` system, any `0031`…`0033` play slots | Matching `save1.txt`…`save3.txt`, plus each slot's original decoded JSON |

CR1 and Omega import flags, script variables, EXP, levels, equipment, gene plates,
learned entries, inventories, money, field positions, treasure draws, encounter
switches and walking EXP. The original stores permanent stat bonuses together;
the converter preserves their **total**, subtracting the port's automatic level
growth so bonuses are not counted twice. The displayed division between seed
points and skill boosts cannot be recovered separately. CR1 supports level 100.

CR2 imports its system flags and gameplay data, including party/equipment,
Sopia levels/EXP, inventory, Oz, treasure state, story flags and field position.
Its original binary reader also accepts the Vita's shorter system file and finds
`SAVEDATA0020.bin` beside a Vita play/suspend file given with `--game-save`.

CR3 imports each slot's story flags, work variables, names, team level, party,
field position, encounter switch, money, inventory, equipment and attachments,
genes and stock, gene commands/stat additions, item sheets, presents, quests,
VS progress, records and dungeon treasure boxes. Gene identifiers are made unique
across the active list and stock, and equipment references are reconstructed from
the original character tags. Loading a slot keeps that slot's story flags.
Use its title screen to choose a slot; `--continue` loads slot 1 directly.

The converter targets state represented by the ports. Vita trophies, online/social
metadata and alternate VS party loadouts are not activated in the ports. Original
CR3 fields not represented by the port (including those records) remain in the
`vita-slotN.json` sidecars. Equipment copies with the same master ID share the
port's attachment state; the port does not model each duplicate separately.
The import is one-way and does not modify or export back to Vita saves.

## Checks

```sh
python3 tools/test_import_vita_save.py
```

The native importer is tested with `ctest --test-dir build --output-on-failure`
after building. These checks cover native confirmation/cancel behavior, checksum
failures, CR3 encodings, malformed input, backups and failed installations.

The Python tests use synthetic data for checksum failures, malformed compressed files,
CR3's two encoding layers, inventory formats, permanent bonuses and replacement
backups. The supplied real export was additionally checked against the native
save readers and field renderers. No proprietary save data is included in this repository.
