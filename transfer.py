#!/usr/bin/env python3
"""Copy the SIMON65 folder to a MEGA65's SD card over ethernet.

Uses mega65_ftp from mega65-tools (or M65Connect). The machine resets when the copy ends;
then start the game from BASIC:

    CHDIR "SIMON65",U12
    RUN "SIMON65.PRG",U12

Save slots already on the card are never overwritten. SPEECH.BIN (171 MB, several minutes)
is skipped when the card holds it at the same size; every other file goes each time.

    python3 transfer.py [SIMON65] [--tools DIR]
"""

from __future__ import annotations

import argparse
import logging
import os
import shutil
import subprocess
import sys
from pathlib import Path

log = logging.getLogger("transfer")

FTP_NAMES = ("mega65_ftp", "mega65_ftp.exe", "mega65_ftp.osx")
TOOLS_ENV = "M65_TOOLS"

SAVE_NAMES = [f"SIMON1.{slot:03d}" for slot in range(4)]

# Too large to send every time, and made from a release that does not change.
SENT_ONCE = {"SPEECH.BIN"}

# Commands per mega65_ftp run: one run with every file stopped partway, at about five
# hundred commands, with no error.
BATCH = 200


def default_folder() -> Path:
    """SIMON65/ beside this script in a release, else the build's in a source checkout."""
    here = Path(__file__).resolve().parent
    release = here / "SIMON65"
    return release if release.is_dir() else here / "build" / "SIMON65"


def find_ftp(tools: Path | None) -> Path | None:
    """mega65_ftp from @p tools, then $M65_TOOLS, then PATH, by any of its names.

    M65Connect keeps it in a subdirectory, so both DIR and DIR/m65ftp are searched.
    """
    dirs = [d for d in (tools, os.environ.get(TOOLS_ENV)) if d]
    search = [str(p) for d in dirs for p in (Path(d), Path(d) / "m65ftp")]
    for name in FTP_NAMES:
        for path in [*search, None]:
            found = shutil.which(name, path=path)
            if found:
                return Path(found)
    return None


def listed_size(listing: str, name: str) -> int | None:
    """The size a mega65_ftp `dir` listing gives @p name, or None if absent.

    A file's line is size | date | 8.3 name | long name (mega65_ftp.c, show_directory).
    """
    for line in listing.splitlines():
        fields = [f.strip() for f in line.split("|")]
        if len(fields) >= 4 and name.upper() in (f.upper() for f in fields[2:4]):
            digits = "".join(c for c in fields[0] if c.isdigit())
            return int(digits) if digits else 0
    return None


def to_send(folder: Path, listing: str | None) -> list[Path]:
    """The files of @p folder to put, given the card's listing (None if unknown)."""
    files = sorted(f for f in folder.iterdir() if f.is_file())
    if listing is None:
        # Without a listing, a save on the card cannot be told apart: keep them all.
        log.warning("could not list the card; save slots are left alone")
        return [f for f in files if f.name.upper() not in SAVE_NAMES]
    kept = []
    for f in files:
        on_card = listed_size(listing, f.name)
        if f.name.upper() in SAVE_NAMES and on_card is not None:
            log.info("%s on the card: kept", f.name)
        elif f.name.upper() in SENT_ONCE and on_card == f.stat().st_size:
            log.info("%s on the card at its size: skipped", f.name)
        else:
            kept.append(f)
    return kept


def run(ftp: Path, folder: Path, commands: list[str]) -> subprocess.CompletedProcess:
    """mega65_ftp over ethernet, run from @p folder so `put` takes bare names."""
    args = [str(ftp), "-e", "-y"]
    for c in commands:
        args += ["-c", c]
    log.debug("%s", " ".join(args))
    return subprocess.run(args, cwd=folder, capture_output=True, text=True)


def transfer(folder: Path, ftp: Path, card_dir: str) -> int:
    """Copy @p folder into @p card_dir on the card; 0 on success."""
    # mkdir fails harmlessly when the directory is already there.
    head = [f"mkdir {card_dir}", f"cd {card_dir}"]
    listing = run(ftp, folder, [*head, "dir"])
    files = to_send(folder, listing.stdout if listing.returncode == 0 else None)
    if not files:
        log.info("nothing to send")
        return 0
    total = sum(f.stat().st_size for f in files)
    log.info("%d files, %.1f MB -> %s/", len(files), total / 1e6, card_dir)

    puts = [f"put {f.name}" for f in files]
    for at in range(0, len(puts), BATCH):
        batch = puts[at : at + BATCH]
        last = at + BATCH >= len(puts)
        log.info("files %d-%d of %d", at + 1, at + len(batch), len(puts))
        # Only the last run says `exit`, which resets the machine.
        done = run(ftp, folder, [*head, *batch, *(["exit"] if last else [])])
        log.debug("%s", done.stdout)
        if done.returncode != 0:
            log.error(
                "mega65_ftp failed (%d): %s", done.returncode, done.stderr.strip()
            )
            return done.returncode
    log.info("done; the machine has reset")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "folder",
        type=Path,
        nargs="?",
        default=default_folder(),
        help="the folder to copy (default: the SIMON65 folder)",
    )
    ap.add_argument(
        "--tools", type=Path, help=f"where mega65_ftp is (else ${TOOLS_ENV}, PATH)"
    )
    ap.add_argument(
        "-v", "--verbose", action="store_true", help="show mega65_ftp's output"
    )
    ap.add_argument("-q", "--quiet", action="store_true", help="report only problems")
    a = ap.parse_args()
    level = logging.DEBUG if a.verbose else logging.WARNING if a.quiet else logging.INFO
    logging.basicConfig(level=level, format="%(levelname)s: %(message)s")

    if not a.folder.is_dir():
        log.error("no folder %s: build, then run extract.py", a.folder)
        return 1
    if not (a.folder / "SIMON65.PRG").is_file():
        log.warning("no SIMON65.PRG in %s: build first", a.folder)
    ftp = find_ftp(a.tools)
    if ftp is None:
        log.error("no mega65_ftp found: pass --tools DIR or set %s", TOOLS_ENV)
        return 1
    return transfer(a.folder.resolve(), ftp, a.folder.resolve().name.upper())


if __name__ == "__main__":
    sys.exit(main())
