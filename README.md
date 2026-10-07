# simon65

*Simon the Sorcerer* for the MEGA65, played from the original Amiga CD32 data. It is a
new interpreter for the game's AGOS bytecode, written in LLVM-MOS C++ with ScummVM's
`engines/agos` as the format reference. No game data is included: you need your own CD32
disc, and the data directory on it (with `gameamiga`, `runit2`, `icon.pkd` and the
`*.out`, `*tune`, `*simon` and `*Effects` files).

## From a release

Download `simon65-<version>.zip` from the releases page and unzip it. With Python 3:

```sh
python3 extract.py /path/to/cd32
```

This converts the game's data into the `SIMON65` folder beside the programs. Copy that
folder to the root of the MEGA65's SD card, or, with the MEGA65 on your network, run
`python3 transfer.py` to copy it over ethernet (it needs `mega65_ftp` from mega65-tools;
pass `--tools DIR` if that is not on your `PATH`). Save slots already on the card are
kept.

Then, on the MEGA65:

```
CHDIR "SIMON65",U12
RUN "SIMON65.PRG",U12
```

Without `U12`, BASIC reads drive 8, not the SD card, and `.PRG` must be given. If the
screen stays black with a purple border, the expansion RAM did not come up: switch the
MEGA65 off and on rather than resetting it.

## From source

You need the llvm-mos compiler and the llvm-mos SDK with the `mega65-banked-nokernal`
platform (LINKS TBD), CMake 3.20 or newer, Ninja or Make, and Python 3. CMake finds the
toolchain through `-DLLVM_MOS=...` and `-DMEGA65_SDK=...`, or environment variables of
the same names.

```sh
cmake -B build
cmake --build build
python3 extract.py /path/to/cd32
```

`build/SIMON65` is then the folder to copy, as above. `cmake --build build --target
release` makes the release zip.

## Keys

| Key | |
|---|---|
| Esc | skip a cutscene, where the game allows it |
| F1 / F3 / F5 / F7 | load slot 0 / 1 / 2 / 3 |
| F2 / F4 / F6 / F8 | save slot 0 / 1 / 2 / 3 (Shift + F1, F3, ...) |
| F | fast-forward on and off |

Slot 0 is the one the game's own postcard saves and loads. Saving and loading wait for
the pointer to show; the border flashes to say whether it worked.

## Contributing

See `CONTRIBUTING.md`.

## Licence

GPL-3.0-or-later; see `LICENSE`. A release's programs are built from the tagged source of
the same version.
