#!/usr/bin/env python3
"""Convert the Amiga CD32 release of Simon the Sorcerer into the SIMON65 folder.

Reads the CD32 data directory and writes, beside the programs the build puts there, every
file the game opens: the database and tables, one file per zone, both fonts, speech,
effects, music, icons and four blank save slots. Copy the folder to the SD card as it is.

    python3 extract.py /path/to/cd32 [--out SIMON65]

Standard library only. Formats are cited to ScummVM's engines/agos sources.
"""

from __future__ import annotations

import argparse
import logging
import shutil
import struct
import sys
from pathlib import Path

log = logging.getLogger("extract")


# --- files copied as they are --------------------------------------------------------------

# The game matches FAT 8.3 names; "gameamiga" is nine characters, so it goes on as GAMEDATA.
CARD_NAME = {"gameamiga": "GAMEDATA"}


def default_folder() -> Path:
    """SIMON65/ beside this script in a release, else the build's in a source checkout."""
    here = Path(__file__).resolve().parent
    release = here / "SIMON65"
    return release if release.is_dir() else here / "build" / "SIMON65"


def wanted(data: Path) -> list[Path]:
    """The release's files the game opens by name.

    0119.out is zone 11's pixels with the beard on; it replaces the pixel half of 011.zon
    when the game asks for it (loadVGABeardFile, res.cpp:763).
    """
    files = [
        data / "gameamiga",
        data / "tbllist",
        data / "stripped.txt",
        data / "0119.out",
    ]
    files += sorted(data.glob("TABLES[0-9][0-9]"))
    files += sorted(data.glob("TEXT[0-9][0-9]"))
    return [f for f in files if f.is_file()]


def card_name(path: Path) -> str:
    """The upper-case 8.3 name a release file goes on the card under."""
    return CARD_NAME.get(path.name, path.name).upper()


# --- zones ---------------------------------------------------------------------------------

# A joined zone's header: the script half's length, little-endian, then two spare bytes
# that keep the pixels on an even address.
ZONE_HEADER = struct.Struct("<HH")
SCRIPT_LIMIT = 0xFFFF


def join_zones(data: Path, into: Path) -> list[Path]:
    """One NNN.ZON per zone: header, scripts, pixels.

    The release ships each zone as two files. Opening a file on the card scans the
    directory inside one uninterruptible Hyppo call, so one file per zone halves the scans
    a room costs and shortens every scan.
    """
    made = []
    for scripts in sorted(data.glob("[0-9][0-9][0-9]1.out")):
        zone = scripts.name[:3]
        pixels = data / f"{zone}2.out"
        body = scripts.read_bytes()
        if len(body) > SCRIPT_LIMIT:
            raise SystemExit(
                f"{scripts.name} is {len(body)} bytes, over the {SCRIPT_LIMIT} a header holds"
            )
        out = into / f"{zone}.ZON"
        with out.open("wb") as f:
            f.write(ZONE_HEADER.pack(len(body), 0))
            f.write(body)
            if pixels.is_file():
                f.write(pixels.read_bytes())
        made.append(out)
    return made


# --- fonts ---------------------------------------------------------------------------------
#
# Both are lifted from the game's executable, runit2, found by their first glyphs rather
# than at an address, so a differently built binary still works.
#
# FONTWIN.BIN: what windowDrawChar reads (charset-fontdata.cpp:2917). 98 glyphs of six by
# eight, a bit a pixel and a byte a row, from character 32.
#
# FONTSAY.BIN: what renderStringAmiga reads (:1101), the font actors speak in. 90
# characters of 41 bytes: ten rows of four masks, then a width. Three masks are shading at
# the script's colour plus 0, 1 and 2; the fourth is the outline.

GLYPHS = 98
LINES = 8
FIRST_CHAR = 32

# Space, then '!': sixteen bytes that occur once in the executable.
ANCHOR = bytes(LINES) + bytes.fromhex("2070702020002000")

SAY_CHARS = 90
SAY_BYTES = 41
SAY_FIRST_CHAR = ord("!")

# The speech font's '!': an outline around a bar, then its width.
SAY_ANCHOR = bytes.fromhex(
    "0000002000002050201040883040008820000050200000500000002000002050000000200000000005"
)

WINDOW_FILE = "FONTWIN.BIN"
SPEECH_FONT_FILE = "FONTSAY.BIN"


def _find_once(image: bytes, anchor: bytes, size: int, what: str) -> bytes:
    """The @p size bytes from the one place @p anchor occurs in @p image."""
    at = image.find(anchor)
    if at < 0:
        raise SystemExit(f"no {what} in that file")
    if image.find(anchor, at + 1) >= 0:
        raise SystemExit(f"the {what} pattern occurs more than once")
    end = at + size
    if end > len(image):
        raise SystemExit(f"the file ends {end - len(image)} bytes into the {what}")
    return image[at:end]


def find_font(image: bytes) -> bytes:
    """The windows' font table."""
    return _find_once(image, ANCHOR, GLYPHS * LINES, "window font")


def find_speech_font(image: bytes) -> bytes:
    """The proportional speech font table."""
    return _find_once(image, SAY_ANCHOR, SAY_CHARS * SAY_BYTES, "speech font")


def make_fonts(datadir: Path, into: Path, binary: str = "runit2") -> list[Path]:
    """Both fonts, lifted from @p binary."""
    image = datadir / binary
    if not image.is_file():
        raise SystemExit(f"no {image}: the fonts are lifted from it")
    data = image.read_bytes()
    made = []
    for name, font in (
        (WINDOW_FILE, find_font(data)),
        (SPEECH_FONT_FILE, find_speech_font(data)),
    ):
        out = into / name
        out.write_bytes(font)
        made.append(out)
    return made


# --- speech and effects --------------------------------------------------------------------
#
# Kept as the CD32 has them: signed 8-bit, speech at 22050 Hz, effects mixed into the
# speech stream at its rate (runit2 0x1ccce). Nothing is resampled; this only reorganises.
#
# SPEECH.BIN  every distinct voice once, each from a sector boundary and padded with
#             silence to the next, so it is read off the card in whole sectors.
# SPEECH.IDX  per voice id: first sector and length in bytes, little-endian 32-bit each;
#             zero for none. One index serves every set: no id has two different voices.
# SETnn.BIN   set nn's effects, loaded by script opcode 185 (script_s1.cpp:544-554): a
#             header of 141 entries, first page and page count, little-endian 16-bit,
#             zero for none; then each distinct clip from a page boundary, padded.
#
# The release, big-endian (sound.cpp:64-99): NNsimon is a table of offsets, its size the
# second entry, then at each a 32-bit length and signed PCM (:236-249, :477). NNEffects is
# the same table over VOC files (:450-454), an effect's id its place in the table
# (:519-525). Each VOC holds one sound-data block whose 24-bit length counts the rate and
# codec bytes before the samples.

SECTOR = 512
PAGE = 256
VOICES = 3624  # atticmap::VOICES
VOICE_ENTRY = struct.Struct("<II")
EFFECT_ENTRY = struct.Struct("<HH")
EFFECT_IDS = 141
HEADER_PAGES = -(-EFFECT_IDS * EFFECT_ENTRY.size // PAGE)
VOC_MAGIC = b"Creative Voice File"
VOC_DATA_OFFSET = 0x14  # where the first block's offset is kept
VOC_SOUND_DATA = 1
VOC_RATE_AND_CODEC = 2


def table(data: bytes) -> tuple[int, ...]:
    """A sound file's offset table; its second entry gives the table's size."""
    count = struct.unpack_from(">I", data, 4)[0] // 4
    return struct.unpack_from(f">{count}I", data, 0)


def set_numbers(datadir: Path) -> list[int]:
    """The speech sets the release holds, by number."""
    return sorted(int(p.name[: -len("simon")]) for p in datadir.glob("*simon"))


def voices(datadir: Path) -> list[bytes]:
    """Every voice id's samples, empty where no set holds one.

    An id held by several sets must be the same voice in each, since one index serves
    them all: checked, not assumed.
    """
    said = [b""] * VOICES
    for number in set_numbers(datadir):
        data = (datadir / f"{number}simon").read_bytes()
        for voice, at in enumerate(table(data)[:VOICES]):
            if at + 4 > len(data):
                continue
            size = struct.unpack_from(">I", data, at)[0]
            if size == 0 or at + 4 + size > len(data):
                continue
            samples = data[at + 4 : at + 4 + size]
            if said[voice] and said[voice] != samples:
                raise SystemExit(f"voice {voice} differs between sets")
            said[voice] = samples
    return said


def voc_samples(data: bytes, at: int) -> bytes | None:
    """The samples of the VOC at @p at, or None if there is none."""
    if data[at : at + len(VOC_MAGIC)] != VOC_MAGIC:
        return None
    block = at + struct.unpack_from("<H", data, at + VOC_DATA_OFFSET)[0]
    if data[block] != VOC_SOUND_DATA:
        raise SystemExit(f"VOC at {at:#x}: block type {data[block]}, not sound data")
    size = int.from_bytes(data[block + 1 : block + 4], "little") - VOC_RATE_AND_CODEC
    start = block + 4 + VOC_RATE_AND_CODEC
    return data[start : start + size]


def effects(datadir: Path, number: int) -> list[bytes | None]:
    """Set @p number's effects by id, None where an id has none."""
    data = (datadir / f"{number}Effects").read_bytes()
    found = [voc_samples(data, at) if at < len(data) else None for at in table(data)]
    if len(found) != EFFECT_IDS:
        raise SystemExit(f"{number}Effects has {len(found)} ids, not {EFFECT_IDS}")
    return found


def padded(samples: bytes, unit: int) -> bytes:
    """@p samples padded with silence to a multiple of @p unit."""
    return samples + bytes(-len(samples) % unit)


def place(clips: list[bytes], unit: int, base: int) -> tuple[bytes, list[int | None]]:
    """Each distinct clip once, each from a @p unit boundary, and where each clip went in
    units from @p base; None for an empty clip."""
    body = bytearray()
    placed: dict[bytes, int] = {}
    where: list[int | None] = []
    for samples in clips:
        if not samples:
            where.append(None)
            continue
        if samples not in placed:
            placed[samples] = base + len(body) // unit
            body += padded(samples, unit)
        where.append(placed[samples])
    return bytes(body), where


def speech(said: list[bytes]) -> tuple[bytes, bytes]:
    """SPEECH.BIN and SPEECH.IDX."""
    body, where = place(said, SECTOR, 0)
    index = b"".join(
        VOICE_ENTRY.pack(at, len(samples)) if at is not None else VOICE_ENTRY.pack(0, 0)
        for at, samples in zip(where, said)
    )
    return body, index


def effect_set(found: list[bytes | None]) -> bytes:
    """One SETnn.BIN: the header page, then each distinct clip once."""
    clips = [samples or b"" for samples in found]
    body, where = place(clips, PAGE, HEADER_PAGES)
    header = b"".join(
        EFFECT_ENTRY.pack(at, -(-len(samples) // PAGE))
        if at is not None
        else EFFECT_ENTRY.pack(0, 0)
        for at, samples in zip(where, clips)
    )
    return padded(header, PAGE) + body


def make_sound(datadir: Path, into: Path) -> list[Path]:
    """SPEECH.BIN, SPEECH.IDX and one SETnn.BIN per set."""
    body, index = speech(voices(datadir))
    made = [into / "SPEECH.BIN", into / "SPEECH.IDX"]
    made[0].write_bytes(body)
    made[1].write_bytes(index)
    for number in set_numbers(datadir):
        out = into / f"SET{number:02d}.BIN"
        out.write_bytes(effect_set(effects(datadir, number)))
        made.append(out)
    return made


# --- music ---------------------------------------------------------------------------------
#
# Each tune is a four-channel ProTracker module. Where its sample block lies is written
# into the module's title, which the player never reads, and the player's limits are
# checked here, where a failure names a file instead of hanging the machine.

MOD_TITLE = 20  # bytes of title before the sample headers
MOD_SAMPLES = 31
MOD_SAMPLE_HDR = 30
MOD_ORDERS = 950  # song length, restart, then 128 order bytes
MOD_ORDER_COUNT = 128
MOD_MAGIC = 1080
MOD_PATTERNS = 1084
MOD_PATTERN_SIZE = 1024  # four channels

# FLT8 is absent: its patterns are twice the size and the player is four-channel.
MOD_MAGICS = (b"M.K.", b"M!K!", b"FLT4")

# One 64 KB Attic slot per tune.
TUNE_SLOT = 0x10000

# The chip buffer the samples are copied into (chipmap::SAMPLES_BYTES).
SAMPLES_BYTES = 36864

# The sample block's offset and length, big-endian, in the title (modplay.S).
SAMPLES_AT = ">HH"


class Tune:
    """One module, checked, with the offsets the player needs."""

    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        self.size = len(self.data)
        self.title = self.data[:MOD_TITLE].split(b"\0")[0].decode("latin1")

        magic = self.data[MOD_MAGIC : MOD_MAGIC + 4]
        if magic not in MOD_MAGICS:
            raise ValueError(f"{path}: not a four-channel module (magic {magic!r})")

        lengths = []
        for i in range(MOD_SAMPLES):
            word = struct.unpack_from(
                ">H", self.data, MOD_TITLE + i * MOD_SAMPLE_HDR + 22
            )[0]
            # The player doubles this word in 16 bits and drops the carry.
            if word >= 0x8000:
                raise ValueError(
                    f"{path}: sample {i} length {word} words wraps 16 bits"
                )
            lengths.append(word * 2)

        orders = self.data[MOD_ORDERS + 2 : MOD_ORDERS + 2 + MOD_ORDER_COUNT]
        self.patterns = max(orders) + 1
        self.sample_offset = MOD_PATTERNS + self.patterns * MOD_PATTERN_SIZE
        self.sample_size = sum(lengths)

        # The parts must account for the whole file, or a field above was misread.
        if self.sample_offset + self.sample_size != self.size:
            raise ValueError(
                f"{path}: {self.size} bytes, but header+patterns+samples "
                f"= {self.sample_offset + self.sample_size}"
            )
        if self.size > TUNE_SLOT:
            raise ValueError(
                f"{path}: {self.size} bytes overruns its {TUNE_SLOT} B slot"
            )
        if self.sample_size > SAMPLES_BYTES:
            raise ValueError(
                f"{path}: {self.sample_size} B of samples overruns "
                f"the {SAMPLES_BYTES} B chip buffer"
            )


def tune_name(number: int) -> str:
    """The file the game opens for tune @p number."""
    return f"TUNE{number:02d}.BIN"


def tune_for_card(tune: Tune) -> bytes:
    """The module with its sample block's offset and length in the title."""
    head = struct.pack(SAMPLES_AT, tune.sample_offset, tune.sample_size)
    return head + tune.data[len(head) :]


def make_tunes(data: Path, into: Path) -> list[tuple[Path, Tune]]:
    """Every tune of the release, checked and named by the game's number."""
    made = []
    for path in sorted(data.glob("*tune"), key=lambda p: int(p.name[:-4])):
        tune = Tune(path)
        out = into / tune_name(int(path.name[:-4]))
        out.write_bytes(tune_for_card(tune))
        made.append((out, tune))
    return made


# --- icons ---------------------------------------------------------------------------------
#
# icon.pkd holds 97 icons of 24 by 24 in four planes, run-length coded by row
# (decompressIconPlanar, icons.cpp:88-133). Each is stored as three columns of eight
# pixels, 24 lines of 8 bytes, as full-colour glyphs want them, so drawing one is a few DMA
# jobs. A pixel is 0xF0 | n: the panel's colours start at 224 and an icon's are the upper
# sixteen (runit2 1aeba sets plane 4). Colour 0 is transparent.

SIDE = 24
ROW_BYTES = SIDE // 8
PLANES = 4
PLANE_BYTES = SIDE * ROW_BYTES
PLANAR_BYTES = PLANES * PLANE_BYTES
REPEAT = 0x80  # a control byte at or above this repeats one row
ICONS = 0xF0
ICON_BYTES = SIDE * SIDE  # atticmap ICON_BYTES
ICONS_FILE = "ICONS.BIN"


def unpack_icon(data: bytes, at: int) -> tuple[bytes, int]:
    """One icon's planes from @p at, and where its bytes end."""
    out = bytearray()
    while len(out) < PLANAR_BYTES:
        control = data[at]
        at += 1
        if control < REPEAT:
            n = (control + 1) * ROW_BYTES
            out += data[at : at + n]
            at += n
        else:
            row = data[at : at + ROW_BYTES]
            at += ROW_BYTES
            out += row * (257 - control)
    return bytes(out[:PLANAR_BYTES]), at


def strips(planar: bytes) -> bytes:
    """Four planes as full-colour bytes, one column strip after another."""
    out = bytearray(ICON_BYTES)
    for y in range(SIDE):
        for x in range(SIDE):
            byte, bit = y * ROW_BYTES + x // 8, 7 - x % 8
            n = sum(
                ((planar[p * PLANE_BYTES + byte] >> bit) & 1) << p
                for p in range(PLANES)
            )
            if n:
                out[(x // 8) * SIDE * 8 + y * 8 + x % 8] = ICONS | n
    return bytes(out)


def icons_from_pkd(pkd: bytes) -> bytes:
    """Every icon of @p pkd, in order.

    The table gives only where icons start, so each decode must end exactly where the next
    icon begins; one a byte off would go unnoticed otherwise.
    """
    count = struct.unpack_from(">I", pkd, 0)[0] // 4
    starts = [struct.unpack_from(">I", pkd, 4 * i)[0] for i in range(count)]
    out = bytearray()
    for i, start in enumerate(starts):
        planar, end = unpack_icon(pkd, start)
        expected = starts[i + 1] if i + 1 < count else len(pkd)
        if end != expected:
            raise ValueError(f"icon {i} ends at {end}, the next starts at {expected}")
        out += strips(planar)
    return bytes(out)


def make_icons(data: Path, into: Path) -> list[Path]:
    """ICONS.BIN from the release's icon.pkd."""
    out = into / ICONS_FILE
    out.write_bytes(icons_from_pkd((data / "icon.pkd").read_bytes()))
    return [out]


# --- saves ---------------------------------------------------------------------------------
#
# The game saves in ScummVM's Simon 1 layout, but only over a file's existing sectors: it
# never changes the FAT. So each slot must already exist at full length. Slot 0 is the one
# the game's postcard uses (o_saveUserGame, script.cpp:808), 1-3 the F keys; names from
# genSaveName (saveload.cpp:85-89).

SAVE_SLOTS = 4  # main.cpp SAVE_SLOTS
SAVE_NAMES = [f"SIMON1.{slot:03d}" for slot in range(SAVE_SLOTS)]
SAVE_BYTES = 3584  # atticmap::SAVEGAME_BYTES, seven sectors


def make_saves(into: Path) -> list[Path]:
    """A blank file for each save slot not already in @p into; existing saves are kept."""
    made = []
    for name in SAVE_NAMES:
        out = into / name
        if out.exists():
            log.info("%s kept", name)
            continue
        out.write_bytes(bytes(SAVE_BYTES))
        made.append(out)
    return made


# --- all of it -----------------------------------------------------------------------------


def extract(datadir: Path, into: Path) -> list[Path]:
    """Every file the game reads, from the release in @p datadir into @p into."""
    files = wanted(datadir)
    if not files:
        raise SystemExit(f"no game data under {datadir}")
    into.mkdir(parents=True, exist_ok=True)

    made = []
    for f in files:
        out = into / card_name(f)
        shutil.copyfile(f, out)
        made.append(out)
    steps = (
        ("zones", lambda: join_zones(datadir, into)),
        ("fonts", lambda: make_fonts(datadir, into)),
        ("speech and effects", lambda: make_sound(datadir, into)),
        ("music", lambda: [out for out, _ in make_tunes(datadir, into)]),
        ("icons", lambda: make_icons(datadir, into)),
        ("saves", lambda: make_saves(into)),
    )
    for what, step in steps:
        log.info("%s", what)
        written = step()
        for out in written:
            log.debug("  %-12s %10d", out.name, out.stat().st_size)
        made += written
    return made


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("datadir", type=Path, help="the CD32 release's data directory")
    ap.add_argument(
        "--out",
        type=Path,
        default=default_folder(),
        help="where to write (default: the SIMON65 folder)",
    )
    ap.add_argument(
        "-v", "--verbose", action="store_true", help="list every file written"
    )
    ap.add_argument("-q", "--quiet", action="store_true", help="report only problems")
    a = ap.parse_args()
    level = logging.DEBUG if a.verbose else logging.WARNING if a.quiet else logging.INFO
    logging.basicConfig(level=level, format="%(levelname)s: %(message)s")

    try:
        made = extract(a.datadir, a.out)
    except (SystemExit, ValueError) as e:
        log.error("%s", e)
        return 1
    total = sum(f.stat().st_size for f in made)
    log.info("%d files, %.1f MB -> %s", len(made), total / 1e6, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
