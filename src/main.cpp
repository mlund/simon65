// SPDX-License-Identifier: GPL-3.0-or-later

// Simon the Sorcerer on the MEGA65: the interrupt, and what runs under it.
//
// Here: start-up, the interrupt, the keys, the world's clock, the main loop,
// the backdrop's painting and the verb bar's hooks. The room, the frame, the
// line of speech, the sound and the saves are modules of their own (room,
// frame, speech, talk, saves), each with the script hooks it implements. A
// header state lives in is `inline`, so every translation unit shares one.
//
// The game's own script drives everything: it paints the room, animates the
// sprites over it and says what is said. What cannot be seen on the screen is
// read back out of report::counts instead.

#include "banks.hpp"
#include "chipmap.hpp"
#include "cursor.hpp"
#include "diagnostics.hpp"
#include "display.hpp"
#include "frame.hpp"
#include "hitareas.hpp"
#include "inventory.hpp"
#include "mouse.hpp"
#include "planar.hpp"
#include "room.hpp"
#include "saves.hpp"
#include "sound.hpp"
#include "speech.hpp"
#include "talk.hpp"
#include "target_hooks.hpp"
#include "text.hpp"
#include "verbbar.hpp"
#include "vmstate.hpp"
#include "windows.hpp"

#include <mega65.h>

/// Declared in display.hpp; extern "C" so the monitor finds it.
extern "C" volatile uint16_t frames = 0;

// The one copy of each, where layout.ld puts them. Declared in vmstate.hpp and
// defined here, because a definition in a header lives only until the second
// translation unit.
// `used` as well as `section`: until the loader references them, link-time
// optimisation drops them and the reservation silently does not happen.
namespace vmstate {
[[gnu::section(".gamedb"), gnu::used]] uint8_t database[DATABASE_BYTES];
[[gnu::section(".vmstate"), gnu::used]] agos::VgaVm animation;
[[gnu::section(".vmstate"), gnu::used]] agos::ScriptVm script;
} // namespace vmstate

/// The store's question about the animation VM, answered where the one VM
/// the machine has is defined.
namespace agos {
bool zone_in_use(uint8_t zone) {
    return vmstate::animation.zone_running(zone);
}
} // namespace agos

/// The door into bank 1. Everything VgaVm::execute reaches is inlined into it,
/// so the bank holds the dispatch and nothing in the fixed region calls into it
/// except through banked_call.
extern "C" CODE_BANK(AGOS_VGA_BANK) void vga_execute_banked() {
    vmstate::animation.execute_pending();
}

/// And the tick that drives it, in a bank of its own: the dispatch alone is
/// most of one.
extern "C" CODE_BANK(AGOS_VGA_TICK_BANK) void vga_tick_banked() {
    vmstate::animation.tick_now();
}

/// The script dispatch, likewise: 188 opcodes of it.
extern "C" CODE_BANK(AGOS_SCRIPT_BANK) void script_execute_banked() {
    vmstate::script.execute_pending();
    // Where the script is, published from inside the bank because the fixed
    // region has not the bytes to spare: a stalled script has no other way to
    // say which subroutine it stalled in.
    report::counts[report::IN_SUB] = vmstate::script.in_sub();
    report::counts[report::STOPPED_ON] = vmstate::script.stopped_on();
}

/// Clearing three hundred entries is not small either.
extern "C" CODE_BANK(AGOS_STORE_BANK) void vga_reset_banked() {
    vmstate::animation.clear();
}

/// One arena entry, recorded where the monitor can read it after the fact.
/// Not banked: its caller is the decoder, whose bank has little to spare, and
/// a jsr into the fixed region costs three bytes of it.
namespace trace {
void log_entry(const uint8_t (&row)[RECORD]) {
    const uint16_t n = agos::far_read16_le(AT);
    agos::far_write(
        AT + COUNT_BYTES + agos::Place{static_cast<uint16_t>(n & (RECORDS - 1))} * RECORD,
        row,
        RECORD);
    agos::far_write16_le(AT, static_cast<uint16_t>(n + 1));
}
} // namespace trace

/// What the run amounts to, where the monitor can read it.
namespace report {
[[gnu::used]] volatile uint16_t counts[SLOTS];
}

/// The display palette's shadow, in the reserved low memory: 768 bytes the
/// fixed region has not got, written by SET_PALETTE and read by the frame.
namespace agos {
[[gnu::section(".vmstate"), gnu::used]] uint8_t shadow_palette[SHADOW_BYTES];
}

/// The files the game has in memory, and which they are. In low memory, not
/// beside the database: it is a bookkeeper, not part of the VM that walks the
/// database, and the fixed region has no room for it. open() empties it, which
/// a NOLOAD section needs.
namespace agos {
[[gnu::section(".vmstate"), gnu::used]] GameStore store;
}

namespace {

/// The typing event queue (iomap.txt:517-522). Reading takes the ASCII code at
/// the top of it; any write drops that event and lets the next one through.
struct M65Keyboard {
    uint8_t asciikey;  //!< $D610 UARTMISC:ASCIIKEY
    uint8_t modifiers; //!< $D611 shift, control and mega key state
};
#define KEYBOARD (*(volatile struct M65Keyboard*)0xd610)

/// The 6510-style CPU port direction register (iomap.txt:2). Writing 65 forces
/// 40.5 MHz whatever the speed bits say. The ROM may leave the machine slow and
/// nothing else here sets the speed, so every timing this program reports would
/// be meaningless without it.
#define CPU_PORT_DDR (*(volatile uint8_t*)0x0000)
constexpr uint8_t CPU_FORCE_40MHZ = 0x41;

constexpr uint8_t VIC_IRQ_RASTER = 0x01;

/// Where the game's files live on the card.
constexpr const char* GAME_DIRECTORY = SIMON_CARD_DIR;

/// Border codes for the things that stop a run before it starts.
enum : uint8_t {
    FAULT_NO_DIRECTORY = 2,
    FAULT_NO_DATABASE = 3,
    FAULT_NO_ATTIC = 4,
    FAULT_TUNES = 5,
    FAULT_NO_ROOM = 6,
    FAULT_NO_FONT = 7,
    FAULT_NO_ICONS = 8
};

/// How many timed-out syncs the report keeps, which is all of them so far.
constexpr uint16_t SYNC_LOST_KEPT = 4;

/// The variable a script leaves a subroutine id in for the loop to run
/// (input.cpp:368).
constexpr uint8_t SCRIPT_ASKS = 254;

/// The frames the world has been turned for, against `frames`, which the
/// interrupt counts. A count rather than a flag: a draw that runs past a frame
/// boundary would otherwise lose that frame, and with it game time -- one to
/// seventeen frames a second in the pot room, with the machine mostly idle.
uint16_t frames_taken = 0;

/// The most frames one turn makes up after a long one; the rest are dropped,
/// so a stall costs a slip and never a fast-forward afterwards. The CD32
/// catches up at most two of its ticks after a draw (runit2 0x1bd48), which is
/// two of our periods of two and a half frames.
constexpr uint8_t CATCH_UP_FRAMES = 5;

} // namespace
extern "C" [[gnu::used]] const agos::PackBits PACK_TABLE = agos::PACK;
namespace {

/// The animation VM's rate against the frame's: 50 ms a tick over 20 ms a
/// frame is two ticks in five frames.
constexpr uint8_t VGA_FIFTHS_PER_FRAME = 2;
constexpr uint8_t FIFTHS_PER_VGA_TICK = 5;
uint8_t vga_fifths = 0;

/// Fast-forward, toggled: the game's clock runs as fast as the card and the
/// decoders allow. Every period still runs; none is skipped.
constexpr uint8_t HURRY_KEY = 'f';
constexpr uint8_t HURRY_PERIODS = 8;
volatile uint8_t hurry = 0;

/// Escape, which ends a cutscene the script has said may be ended. The
/// engine's own skip (_exitCutscene, input.cpp:574), not a debugging one.
constexpr uint8_t ESCAPE_KEY = 0x1B;
volatile uint8_t exit_cutscene = 0;

/// The bit a script sets while its cutscene may be skipped, and the
/// subroutine that ends one (script.cpp:1102; endCutscene,
/// subroutine.cpp:258).
constexpr uint16_t CUTSCENE_BIT = 9;
constexpr uint16_t END_CUTSCENE_SUB = 170;

/// A skip taken in a sync wait, whose subroutine the loop runs once the
/// waiting script has unwound. The engine runs it inside the wait; here that
/// would nest a whole chain (170, an item's, 7, 5) inside the wait's door and
/// overrun the soft stack.
uint8_t cutscene_ended = 0;

/// The boxes a click will be tested against.
agos::HitAreas boxes;

/// Where the button went down, and where the pointer is, for the doors
/// below to pick up.
uint16_t clicked_x = 0, clicked_y = 0;
uint16_t pointer_x = 0, pointer_y = 0;

/// How long a script waits for the animation VM before giving up. The
/// engine's own figure, counted in the same ticks (waitForSync,
/// script.cpp:1067). It is not margin: the intro's script waits on 15098,
/// which its animation only sends 482 ticks in, and a shorter wait puts the
/// two machines out of step for the rest of the game -- the script sends the
/// sync the animation is about to wait for, and both stop.
constexpr uint16_t SYNC_MOST_TICKS = 1000;

extern "C" void click_banked();
extern "C" void pointer_banked();
extern "C" void do_icons_banked();
extern "C" void pick_object_banked();
/// The second item getDollar2 was given, for the door's caller.
uint16_t picked_item = 0;
extern "C" void clear_icons_banked();
/// What DO_ICONS asked for, for the verb bank's door.
uint16_t icons_item = 0;
uint8_t icons_window = 0;
/// Where the game keeps its inventory: every DO_ICONS names it, and the
/// engine redraws it there after a load (saveload.cpp:162).
constexpr uint8_t INVENTORY_WINDOW = 2;
extern "C" void prefixes_banked();

/// The text windows below the picture, and what is written in them.
agos::Windows windows;

/// What the game's own load does after LOAD_USER_GAME, which the load key
/// copies: subroutine 141 sets bit 97, and 100, which runs after every click,
/// sees it and re-enters the player's room through subroutine 7 (gameamiga
/// subroutines 141 and 100).
constexpr uint16_t RELOADED_BIT = 97;
constexpr uint16_t RELOADED_MASK = 1U << (RELOADED_BIT % 16);
constexpr uint16_t AFTER_CLICK_SUB = 100;

/// The animation VM runs at 20 Hz, so twenty of its ticks are a second --
/// which is the unit ADD_TIMEOUT counts in.
constexpr uint8_t TICKS_A_SECOND = 20;
uint8_t tick_of_second = 0;
uint32_t script_seconds = 0;

#define IRQ_VECTOR (*(volatile uint16_t*)0xfffe)

/// Stop, with the border carrying the code. There is nowhere better to report
/// to: this runs with no KERNAL and no character set.
[[noreturn]] void fault(uint8_t code) {
    VICIV.bordercol = code;
    for (;;)
        ;
}

} // namespace

extern "C" {
// The refill's two pointers (sound.hpp), kept: inline assembly is their main reader.
[[gnu::used]] __zp volatile uint8_t sound_effect[4];
[[gnu::used]] __zp volatile uint8_t sound_ring[4];
}

/// Called from irq_entry, which has already put B and Z where compiled code
/// needs them and will put back what the interrupted code had.
///
/// interrupt_norecurse makes this an interrupt root, so the static-stack
/// analysis keeps its frames apart from the code it interrupts; without it an
/// asynchronous call into C is undefined. no_isr: the save and the rti are
/// irq_entry's.
extern "C" __attribute__((interrupt_norecurse, no_isr)) void irq_tick() {
    VICII.irr = VIC_IRQ_RASTER; // acknowledge, or it re-fires on the way out
    frames = frames + 1;        // ++ on a volatile is deprecated in C++23
    // An interrupt held up into the next picture leaves the swap for the next.
    if (swap_to != 0 && beam_outside_picture()) {
        display_point(static_cast<uint8_t>(swap_to - 1));
        swap_to = 0;
    }
    tune_store_tick();
    sound::tick();
}

/// The raster interrupt's front door.
///
/// Assembly rather than an interrupt attribute: the attribute cannot see what
/// pep_play clobbers, so it spills the whole imaginary register file -- 142
/// instructions -- in the one hot path this program has. This saves A, X, Y, Z,
/// B and the caller-saved imaginary registers __rc2-__rc19, the ones irq_tick
/// may use without restoring; it restores the rest itself, as any function does.
///
/// B and Z are put back as they were found rather than zeroed, which is what
/// lets a long decode run with interrupts on, and so what keeps the music
/// playing through it.
extern "C" void irq_entry();
asm(R"(
	.section .text.irq,"ax",@progbits
	.global irq_entry
irq_entry:
	pha
	phx
	phy
	phz
	tba
	pha
	lda #0x00
	tab                              ; the compiler's base page
	ldz #0x00                        ; and the Z it is entitled to
	ldx #17                          ; __rc2-__rc19, the caller-saved ones
1:	lda mos8(__rc2),x
	pha
	dex
	bpl 1b
	jsr irq_tick                     ; the handler proper, above
	ldx #0
2:	pla
	sta mos8(__rc2),x
	inx
	cpx #18
	bne 2b
	pla
	tab                              ; whatever the interrupted code had
	plz
	ply
	plx
	pla
	rti
)");

/// The Attic bank answering for itself.
///
/// It holds what fires on a scene change, where Attic code, about six times
/// slower than chip, costs nothing. Nothing
/// proves a bank loaded except asking it something only it knows: a bank that
/// did not load runs whatever bytes sit at that address in the mapped bank.
extern "C" CODE_BANK(AGOS_EXTRA_BANK) void extra_bank_hello() {
    report::counts[report::EXTRA_BANK] = 0xEA7C;
}

/// A line of text on its way to a window, waiting for the door below.
static const char* say_what = nullptr;
static uint8_t say_count = 0;

/// Laying out a line of text is 1,300 bytes and happens when somebody speaks,
/// which is the shape the Attic bank is for: cold, large, and no reason to
/// sit in the region the interpreter runs out of.
extern "C" CODE_BANK(AGOS_EXTRA_BANK) void windows_say_banked() {
    windows.say(say_what, say_count);
}

/// Through the door, from either side of it.
static void say_in_window(const char* s, uint8_t n) {
    say_what = s;
    say_count = n;
    banked_call(AGOS_EXTRA_BANK, windows_say_banked);
}

/// The window in force cleared, its cursor home.
static void clear_window() {
    windows.clear();
    panel_changed = 1;
}

/// The same from another bank, into the sentence line's buffer: banked_call
/// takes no arguments, so the id goes in and the length comes back here.
static uint16_t resolve_id = 0;
static uint8_t resolved = 0;
/// One line of the six-pixel font across the screen: 53 characters.
constexpr uint8_t SENTENCE_CHARS = chipmap::CELLS_ACROSS * chipmap::CELL_LINES / text::WIN_ADVANCE;
static char name_line[SENTENCE_CHARS + 1];

extern "C" CODE_BANK(AGOS_TICK_BANK) void resolve_banked() {
    resolved = speech::resolve(resolve_id, name_line, sizeof name_line);
}

/// The same into the line's buffer, for a window's string.
extern "C" CODE_BANK(AGOS_TICK_BANK) void resolve_shown_banked() {
    resolved = speech::resolve(resolve_id, speech::lent_line(), speech::LINE_CHARS);
}

/// Opening the store runs once and carries the whole table transcoder with it,
/// so it shares bank 3 with the animation VM's reset -- which is 288 bytes and
/// never wanted a bank to itself.
extern "C" CODE_BANK(AGOS_STORE_BANK) void store_open_banked() {
    if (!agos::store.open(vmstate::database, chipmap::DATABASE_BYTES))
        fault(FAULT_NO_DATABASE);
    vmstate::script.reset(agos::store.db(), agos::store.heap());
    vmstate::animation.reset(vmstate::script);
}

/// The three doors into the card reader, which is 4 KB the fixed region has
/// no room for and which never runs in a frame. It shares the animation
/// tick's bank (AGOS_CARD_BANK), which has the room.
extern "C" CODE_BANK(AGOS_CARD_BANK) void card_mount_banked() {
    agos::card_answer = agos::files.mount(agos::card_name) ? 1 : 0;
    agos::card_name = nullptr;
}

extern "C" CODE_BANK(AGOS_CARD_BANK) void card_read_banked() {
    agos::card_answer = agos::files.read(agos::card_name, agos::card_into, agos::card_most);
    agos::card_name = nullptr;
}

extern "C" CODE_BANK(AGOS_CARD_BANK) void card_find_banked() {
    agos::card_entry = agos::files.find(agos::card_name);
    agos::card_name = nullptr;
}

extern "C" CODE_BANK(AGOS_CARD_BANK) void card_next_banked() {
    agos::card_from = agos::files.next_cluster(agos::card_from);
}

extern "C" CODE_BANK(AGOS_CARD_BANK) void card_sectors_banked() {
    agos::card_answer =
        agos::files.read_sectors(agos::card_from, agos::card_into, agos::card_count) ? 1 : 0;
}

extern "C" CODE_BANK(AGOS_CARD_BANK) void card_zone_banked() {
    char name[8];
    agos::zone_name(name, agos::card_zone);
    agos::card_answer = agos::files.read_headed(name,
        agos::ZONE_HEADER,
        {atticmap::slot(atticmap::ZONESCRIPTS, agos::card_slot), 1UL << atticmap::SHIFT},
        {atticmap::PACKED, atticmap::PACKED_BYTES},
        &agos::card_pixels,
        &agos::card_pixel_bytes);
}

/// The ground a new scene starts from: the picture cleared, and every row of
/// it owed a rebuild.
///
/// Said once because two things start a scene. A room's base arrives as an
/// opaque DRAW, and fourteen of the release's backdrops are narrower than the
/// picture, so the base alone does not cover what the last room left beside
/// it. SET_WINDOW_IMAGE arrives with no base at all -- the scene it names is
/// sprites -- so without this the last scene stays underneath it.
static void clear_the_picture() {
    frame::backdrop_changed();
    agos::far_fill(chipmap::BACKDROP, 0, chipmap::PICTURE_BYTES);
    rows_owed(0, chipmap::PICTURE_ROWS);
}

/// A paint that did not happen, counted wherever it fell over: the whole
/// point of the census beside it is to tell a draw that was dropped from one
/// the scripts never made.
static void missed_a_paint() {
    report::count_one(report::PAINT_MISSED);
}

/// SET_WINDOW_IMAGE's half of the work that is not the script: the window's
/// picture is replaced, so the ground goes. In the room's bank because that
/// is where the picture's memory is handled.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void window_image_banked() {
    clear_the_picture();
}

/// One image painted where the script says, into the backdrop either way.
///
/// A room is not one picture: zone 64's script paints twenty-two, the base and
/// then its scenery, each over what is already there. They all belong in the
/// backdrop's glyphs. Keeping one painted figure instead loses every piece but
/// the last: the room stands bare, and the hearth goes black for the ten
/// ticks its fire animation leaves blank, where DRAW 9 painted a burning
/// hearth underneath it.
///
/// Which decoder is the opcode's own answer, not a guess from the image's
/// size. kDFNonTrans (0x2, vga.h:101) says paint every pixel, and measured over
/// the release it is set on 177 DRAWs and never once on a paint whose palette
/// block is not nought -- so opaque means the room's own backdrop, at whatever
/// width the room happens to be, and the rest are scenery laid over it.
///
/// Both decode straight out of PACKED, so PACKED must hold the zone that
/// asked. Which zone that is is not a guess: a DRAW names an image in the zone
/// whose script is drawing, and the last one staged is often another -- one
/// zone's table read against another's pixels would crash on opaque data, so
/// the zone is checked.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void paint_banked() {
    const agos::PackedZones::Pixels pixels = agos::zone_pixels_of(agos::paint_zone);
    if (!pixels.valid()) {
        missed_a_paint();
        return;
    }
    const agos::ImageEntry image = agos::image_entry(pixels.at, agos::paint_image);
    if (!image.plausible()) {
        missed_a_paint();
        return;
    }

    // One call and not two: the flags are what differ, and a second call site
    // means a second copy of the decoder inlined into this bank.
    const bool opaque = (agos::paint_flags & agos::DRAW_OPAQUE) != 0;
    // A room's base is where a room starts, and the scenery painted over it
    // stays until something clears it. Fourteen of the release's backdrops are
    // narrower than the picture, so the base alone does not cover what the last
    // room left beside it.
    //
    // The engine would keep what lies outside a narrow base rather than clear
    // the whole picture. The difference shows only if a script paints a base
    // over a room it has already dressed; measured, 171 of the release's 179
    // opaque DRAWs cover the screen at nought, nought, and a picture runs one
    // image script.
    //
    // Below the picture is the panel, whose paints go to its Attic master and
    // never touch the room: the verb bar arrives as an opaque DRAW through
    // window 5, or window 0 at y 144 (zone 0's image scripts 1 and 0). Chosen
    // by where the piece starts, so the one call site stays.
    //
    // An overlay is drawn over chip PANEL alone, so the master stays what lies
    // under it: the inventory's arrows, taken away by copying the master back.
    const bool panel = agos::paint_y >= int16_t{chipmap::PICTURE_LINES};
    const bool overlay = (agos::paint_flags & agos::PAINT_OVERLAY) != 0;
    if (panel)
        agos::paint_y = static_cast<int16_t>(agos::paint_y - chipmap::PICTURE_LINES);
    if (opaque) {
        if (panel)
            agos::far_fill(atticmap::PANEL_MASTER, 0, atticmap::PANEL_MASTER_BYTES);
        else
            clear_the_picture();
    }
    frame::backdrop_changed();
    // Timed against the interrupt: a decode outlasts a raster by far.
    const uint16_t began = frames_now();
    const bool painted = agos::decode_piece(pixels.at,
        pixels.bytes,
        image,
        overlay     ? chipmap::PANEL
            : panel ? atticmap::PANEL_MASTER
                    : chipmap::BACKDROP,
        chipmap::CELLS_ACROSS,
        panel ? uint8_t{chipmap::SCREEN_ROWS - chipmap::PICTURE_ROWS} : chipmap::PICTURE_ROWS,
        agos::paint_x,
        agos::paint_y,
        agos::paint_block,
        !opaque);
    if (!painted) {
        agos::vga_fault(agos::Fault::VGA_ROOM_UNDECODED, agos::paint_image);
        missed_a_paint();
    }
    report::counts[report::DECODE_FRAMES] = static_cast<uint16_t>(frames_now() - began);
    // A piece changed the picture, so the rows carrying it are owed a rebuild;
    // an opaque base cleared them all above. The panel's master goes down
    // whole: one DMA, and the VIC cannot read Attic.
    if (panel) {
        if (!overlay)
            agos::far_copy(atticmap::PANEL_MASTER, chipmap::PANEL, atticmap::PANEL_MASTER_BYTES);
        rows_owed(chipmap::PICTURE_ROWS, chipmap::SCREEN_ROWS);
    } else {
        rows_owed(0, chipmap::PICTURE_ROWS);
    }
}

/// What world_tick_banked answers: banked_call takes a function of no
/// arguments, so the periods this turn ran come back here, for the script's
/// clock.
static uint8_t ticked = 0;

/// The engine's own name for the flag that gives every other period a third
/// pass of the animation VM (event.cpp:696).
static bool cepe = false;

/// The soft stack, watched at the one place it does damage.
///
/// It grows down from $FFFA through ram_high toward .bss and nothing checks
/// the collision: llvm-mos keeps frames there because the 6502 hardware stack
/// is one page and has no stack-relative addressing (the 45GS02 has both, but
/// code generation does not use them -- llvm-mos issue 286). When it reaches
/// .bss it corrupts whatever object happens to sit at the bottom, so the
/// symptom is a layout accident: 256 more bytes of .bss can turn a clean run
/// into a derailed VM.
///
/// A pattern just above the heap is the last thing the stack touches before
/// the data, and sixteen compares a frame is the whole cost.
///
/// Above __heap_start, not above __bss_end: .noinit lives in between and the
/// program writes it, which a guard there reads as an overflow within the
/// first twenty ticks. Nothing here allocates, so the heap is the free ground
/// the stack descends into.
/// The linker's own name for it, so the spelling is not ours to choose, and
/// an array rather than a char because sixteen bytes are written from it.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming,cert-dcl37-c,cert-dcl51-cpp)
extern "C" char __heap_start[];
constexpr uint8_t GUARD_BYTES = 16;
constexpr uint8_t GUARD_SEED = 0xA5;

static void arm_stack_guard() {
    uint8_t* const guard = reinterpret_cast<uint8_t*>(__heap_start);
    for (uint8_t i = 0; i < GUARD_BYTES; ++i)
        guard[i] = static_cast<uint8_t>(GUARD_SEED ^ i);
}

/// How many of the guard's bytes the stack has taken, nought while it has not.
/// The seed varies per byte so a run of zeros or a copied block counts too.
[[nodiscard]] static uint8_t stack_took() {
    const uint8_t* const guard = reinterpret_cast<const uint8_t*>(__heap_start);
    uint8_t took = 0;
    for (uint8_t i = 0; i < GUARD_BYTES; ++i)
        if (guard[i] != static_cast<uint8_t>(GUARD_SEED ^ i))
            ++took;
    return took;
}

/// FADE_TO_BLACK: waits out eight frames, so in the Attic with what fires
/// on a scene change.
extern "C" CODE_BANK(AGOS_EXTRA_BANK) void fade_to_black_banked() {
    agos::fade_to_black();
}

/// One turn of the world: the animation VM, the screen, and what the frame
/// asked to be fetched. `ticked` is nought when this raster was not one of the
/// 50 ms ticks.
///
/// A script waiting on a sync runs this too. Ticking only the animation VM
/// there would leave the screen on the frame the wait began with -- the intro
/// playing out behind a still picture -- and run it at processor speed
/// besides, so the whole of it goes by in seconds. The engine's wait runs its
/// own event loop for both reasons (waitForSync, script.cpp:1117).
///
/// What stays in the main loop is everything that can start a script: the
/// mouse, the keys, the timeout clock. Inside a wait, the script they would
/// start is the one waiting.
extern "C" CODE_BANK(AGOS_TICK_BANK) void world_tick_banked() {
    ticked = 0;
    const uint16_t now = frames_now();
    auto elapsed = static_cast<uint16_t>(now - frames_taken);
    if (elapsed == 0)
        return;
    frames_taken = now;
    talk::pump();
    saves::flash_over(now);
    // The pointer moves whatever the script is doing: a wait on an animation
    // ticks the world here and nothing else, and the engine's pointer kept
    // moving through those. A press is latched for the loop, not acted on.
    const mouse::Point pointer = mouse::poll();
    cursor::at(pointer.x, pointer.y);
    pointer_x = pointer.x;
    pointer_y = pointer.y;
    if (elapsed > CATCH_UP_FRAMES) {
        report::count_add(report::LOST_FRAMES, static_cast<uint16_t>(elapsed - CATCH_UP_FRAMES));
        elapsed = CATCH_UP_FRAMES;
    }

    // The peak, not the moment: an excursion that recedes before the next tick
    // reads as nought, and this is sampled at tick boundaries only -- so a
    // reading of nought rules out a standing overflow, not a brief one.
    if (const uint8_t took = stack_took(); took > report::counts[report::STACK_GUARD]) {
        report::counts[report::STACK_GUARD] = took;
        if (report::counts[report::FAULTS] == 0)
            agos::script_fault(agos::Fault::STACK_OVERRAN, took);
    }

    // Every key leaves the hardware's queue here, waits included: one left
    // there sits at its head and hides every key behind it, Esc too. Hurry
    // and Esc act at once -- a wait is most of the intro, exactly what they
    // are for -- and the rest are held for the loop.
    if (const uint8_t key = KEYBOARD.asciikey; key != 0) {
        KEYBOARD.asciikey = 0; // any write drops the event
        if (key == HURRY_KEY)
            hurry = hurry == 0 ? 1 : 0;
        else if (key == ESCAPE_KEY)
            exit_cutscene = 1;
        else {
            // Whatever is running ends before a load -- its wait let go and the
            // chain unwound, as a skipped cutscene is -- and the loop loads. Not
            // when the load is refused, which would leave nothing running: so the
            // slot is read and checked first.
            if (saves::press(key))
                vmstate::script.unwind();
        }
    }

    // The animation VM runs at 20 Hz, not at the frame rate: Simon 1 sets
    // _vgaPeriod to 50 milliseconds and the engine ticks once a period
    // (agos.cpp:818, event.cpp:439). A PAL frame is 20 ms, so two ticks fall in
    // every five frames -- counting in fifths keeps that exact rather than
    // drifting a frame at a time.
    // Every frame since the last turn counts, and each whole period of them is
    // run below -- the animation VM's ticks for each, one draw for them all.
    //
    // Not after a held draw: the world waits for the frame, as the CD32's does
    // for a late one, and the frames go as lost. The draw is tried again.
    uint8_t periods = 0;
    if (frame::held()) {
        report::count_add(report::LOST_FRAMES, elapsed);
    } else {
        vga_fifths = static_cast<uint8_t>(vga_fifths + VGA_FIFTHS_PER_FRAME * elapsed);
        while (vga_fifths >= FIFTHS_PER_VGA_TICK) {
            vga_fifths = static_cast<uint8_t>(vga_fifths - FIFTHS_PER_VGA_TICK);
            ++periods;
        }
        if (hurry != 0)
            periods = HURRY_PERIODS;
    }
    if (periods == 0 && !frame::held()) {
        // A turn with no period due is the idle time decode-ahead is for.
        frame::decode_ahead();
        return;
    }

    // Twice a period, and three times on every other one: that is what the
    // engine's timer does (timerProc, event.cpp:693-699), so a period is worth
    // two and a half passes of the animation VM. Ticking it once runs the game
    // at two fifths of its speed, which a player sees as half.
    const Stamp vm_began = stamp();
    for (uint8_t period = periods; period != 0; --period) {
        vmstate::animation.tick();
        vmstate::animation.tick();
        cepe = !cepe;
        if (!cepe)
            vmstate::animation.tick();
    }
    // What the animation VM costs against what the frame costs: the two
    // together are what a period has to fit, and only one of them is the draw.
    count_since(report::TICK_LINES, vm_began);
    const uint16_t vm_took = static_cast<uint16_t>(frames_now() - vm_began.frame);
    report::counts[report::TICK_RASTERS] = vm_took;
    report::note_peak(report::WORST_TICK, vm_took);

    const Stamp before = stamp();
    frame::draw();
    count_since(report::DRAW_LINES, before);
    const uint16_t took = static_cast<uint16_t>(frames_now() - before.frame);
    report::counts[report::FRAME_RASTERS] = took;
    // The peak, not just the last: one draw in a hundred is the one that
    // stalls, and a mean hides it.
    report::note_peak(report::WORST_FRAME, took);
    report::count_one(report::TICKS);

    // The palette the scripts loaded, put up once the frame is built and not
    // while a fade is holding the screen black (displayScreen, draw.cpp:964).
    if (agos::palette_dirty != 0 && agos::palette_held == 0)
        agos::show_palette();

    speech::pick_up();

    room::fetch_wanted();
    // Fast-forward reports one: the script's clock runs at the frame rate
    // there, not eight times it.
    ticked = hurry != 0 && periods != 0 ? 1 : periods;
    // And what is left of this frame, after the draw.
    frame::decode_ahead();
}

/// The turn of the world, through the door: how many periods it ran, nought
/// if none were due.
[[nodiscard]] static uint8_t world_tick() {
    banked_call(AGOS_TICK_BANK, world_tick_banked);
    return ticked;
}

/// The sync WAIT_SYNC asked for, for the door below.
static uint16_t wait_ident = 0;

/// Wait for the animation VM to say it has got there.
///
/// The engine spins its own event loop here (waitForSync) and so does this:
/// the animation VM is ticked until the rendezvous clears. Bounded, because a
/// sync that never comes would otherwise be a machine that has stopped with
/// no way to say why -- the counter says instead.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void wait_sync_banked() {
    report::counts[report::SYNC_WANTED] = wait_ident;
    if (!vmstate::script.wait_for_sync(wait_ident))
        return;
    // An Esc from before the wait is dropped (script.cpp:1080): only one
    // pressed during it skips.
    exit_cutscene = 0;
    for (uint16_t spun = 0; spun < SYNC_MOST_TICKS;) {
        if (!vmstate::script.waiting())
            return;
        // Escape, where the engine takes it: a wait is where a cutscene may be
        // skipped, and the bit is the script saying this one may. The flag
        // unwinds the script that was waiting, and subroutine 170, the game's
        // own ending for it, follows from the loop (endCutscene,
        // subroutine.cpp:253-262; see cutscene_ended).
        if (exit_cutscene != 0) {
            exit_cutscene = 0;
            if (vmstate::script.bit(CUTSCENE_BIT)) {
                vmstate::script.unwind();
                cutscene_ended = 1;
                return;
            }
        }
        if (world_tick())
            ++spun;
    }
    vmstate::script.stop_waiting();
    // Which one, not just how many: the ident names the sprite that never got
    // there, and the scene the script then ran past without.
    const uint16_t lost = report::counts[report::SYNC_GAVE_UP];
    if (lost < SYNC_LOST_KEPT)
        report::counts[report::SYNC_LOST + lost] = wait_ident;
    if (lost == 0)
        speech::note_lost();
    report::counts[report::SYNC_GAVE_UP] = static_cast<uint16_t>(lost + 1);
}

namespace agos {

// In the dispatch's bank, its only caller. Re-entered: the tick runs while
// the walk's frame is live.
CODE_BANK(AGOS_SCRIPT_BANK) void script_rescan() {
    banked_reenter<world_tick_banked>(AGOS_TICK_BANK);
}

void script_wait_sync(uint16_t ident) {
    wait_ident = ident;
    banked_call(AGOS_ROOM_BANK, wait_sync_banked);
}

} // namespace agos

/// The subroutine a script asked for by leaving its id in variable 254.
static uint16_t script_wanted = 0;

extern "C" CODE_BANK(AGOS_SCRIPT_BANK) void run_wanted_banked() {
    // A skip leaves the unwinding flag up; the subroutine it asks for must not
    // inherit it.
    vmstate::script.clear_returning();
    vmstate::script.start(script_wanted);
    vmstate::script.clear_returning();
}

/// What subroutine 141 does after its load that this interpreter has, for a
/// load key's load, which has no script left to do it: bit 97 for subroutine
/// 100 to see -- asked for through variable 254, so the loop runs it next
/// tick, from the top, rather than the save bank starting a script inside
/// itself.
[[gnu::always_inline]] void saves::reloaded() {
    // Whatever scene was playing goes with its script, through KILL_ANIMATE's
    // own door: the saved room's script puts back what belongs.
    room::kill_all();
    // By word, not set_bit: that is the fixed region's, and a second caller in
    // another bank would cost it a copy.
    vmstate::script.bits()[RELOADED_BIT / 16] |= RELOADED_MASK;
    vmstate::script.set_variable(SCRIPT_ASKS, AFTER_CLICK_SUB);
    // And what the engine draws itself after a load (saveload.cpp:162).
    icons_item = agos::PLAYER;
    icons_window = INVENTORY_WINDOW;
    banked_call(AGOS_VERB_BANK, do_icons_banked);
}

/// The verb bar, in an Attic bank of its own: its state is UI state, which
/// lives in Attic, and it runs once a tick or a click, which is cold.
VERB_DATA static agos::VerbBar verb_bar;
/// The inventory, beside the verb bar whose boxes it adds to.
VERB_DATA static agos::Inventory inventory;
VERB_CONST const char agos::VERB_NAMES[] =
    "\7Walk to\7Look at\4Open\4Move\7Consume\7Pick up\5Close\3Use"
    "\7Talk to\6Remove\4Wear\4Give";
VERB_CONST const char agos::VERB_PROMPTS[] = "\0\0\0\0\0\0\0\13with what ?\0\0\0\11to whom ?";

/// The sentence line is text window 1 (DEFINE_WINDOW 1 0 136 40 1).
constexpr uint8_t SENTENCE_WINDOW = 1;

// name_line, the sentence line's text, is declared with the resolver above.
// Its own buffer: the line of speech over the room may be showing.

namespace agos {

CODE_BANK(AGOS_VERB_BANK) void sentence_say(const char* text, uint8_t n) {
    // Off while its ink is nought: a conversation sets that (gameamiga sub 51)
    // and puts 223 back after (sub 52), since its first choice is on this row
    // (FUN_0001aa44 tests the window's ink, which 160 stores at +0x12).
    if (windows.at(SENTENCE_WINDOW).ink == 0)
        return;
    // The script's own window is left as it was: it may be mid-way through
    // writing a line of its own.
    const uint8_t was = windows.current();
    windows.use(SENTENCE_WINDOW);
    clear_window();
    if (n != 0) {
        // Near first: the renderer's bank replaces this one in the window, and
        // a verb's name lives in this one.
        for (uint8_t i = 0; i < n; ++i)
            name_line[i] = text[i];
        // Centred, kept to an even pixel (FUN_00015906, a:1ab0a-1ab18).
        windows.place(static_cast<uint16_t>(((SENTENCE_CHARS - n) * (text::WIN_ADVANCE / 2)) & ~1));
        say_in_window(name_line, n);
    }
    windows.use(was);
}

CODE_BANK(AGOS_VERB_BANK) bool sentence_name(uint16_t item_id) {
    // The item's object names it (aa74 -> 1583e); a room or a null item has no
    // name to give. Every one of the release's 90 object names is a global
    // string, so none is a card read.
    const Item item = vmstate::script.db().item(item_id);
    uint8_t n = 0;
    if (item.valid() && item.is_object())
        n = speech::copy_global(item.object().name(), name_line, sizeof name_line);
    sentence_say(name_line, n);
    return n != 0;
}

CODE_BANK(AGOS_VERB_BANK) bool sentence_text(uint8_t slot) {
    resolve_id = vmstate::script.short_text(slot);
    resolved = 0;
    if (resolve_id != 0)
        banked_call(AGOS_TICK_BANK, resolve_banked);
    const uint8_t n = resolved;
    sentence_say(name_line, n);
    return n != 0;
}

/// Where a click was and which text box it hit (verb.cpp:860; input.cpp:52).
constexpr uint8_t CLICKED_X_VARIABLE = 1, CLICKED_Y_VARIABLE = 2;
constexpr uint8_t CLICKED_TEXT_VARIABLE = 60;

CODE_BANK(AGOS_VERB_BANK) void click_noted(uint16_t x, uint16_t y, uint16_t text_slot) {
    vmstate::script.set_variable(CLICKED_X_VARIABLE, static_cast<int16_t>(x));
    vmstate::script.set_variable(CLICKED_Y_VARIABLE, static_cast<int16_t>(y));
    vmstate::script.set_variable(CLICKED_TEXT_VARIABLE, static_cast<int16_t>(text_slot));
}

/// Zone 1's image 1 at x 38 (cells), y 150, palette block 15, transparent
/// (FUN_0001b57c's operands; icons.cpp:778-785).
constexpr uint8_t ARROWS_ZONE = 1, ARROWS_IMAGE = 1, ARROWS_BLOCK = 15;
constexpr int16_t ARROWS_X = 38, ARROWS_Y = 150;

// The request posted here rather than through vga_paint, whose callers would
// then be in two banks and its copy in the fixed region.
CODE_BANK(AGOS_VERB_BANK) void inventory_arrows() {
    room::load_zone(ARROWS_ZONE);
    paint_zone = ARROWS_ZONE;
    paint_image = ARROWS_IMAGE;
    paint_block = ARROWS_BLOCK;
    paint_x = ARROWS_X;
    paint_y = ARROWS_Y;
    paint_flags = PAINT_OVERLAY;
    banked_call(AGOS_ROOM_BANK, paint_banked);
}

} // namespace agos

/// The icon Inventory::draw is to draw, for the door below.
namespace {
uint16_t icon_asked = 0;
uint8_t icon_x = 0, icon_top = 0;
} // namespace

/// Inventory::draw in its own bank: the verb bar's has no 820 bytes for it, and
/// it runs once an icon a redraw, which is cold.
extern "C" CODE_BANK(AGOS_TICK_BANK) void icon_banked() {
    agos::Inventory::draw(icon_asked, icon_x, icon_top);
}

namespace agos {

CODE_BANK(AGOS_VERB_BANK) void inventory_icon(uint16_t icon, uint8_t x_cell, uint8_t top) {
    icon_asked = icon;
    icon_x = x_cell;
    icon_top = top;
    banked_call(AGOS_TICK_BANK, icon_banked);
}

} // namespace agos

/// The icons put up afresh, and the verb bar told the old ones' boxes went.
CODE_BANK(AGOS_VERB_BANK) static void show_icons(uint16_t owner, uint8_t window, uint8_t line) {
    inventory.show(boxes, vmstate::script.db(), owner, window, windows.at(window), line);
    verb_bar.box_gone(boxes, agos::HitAreas::ICON, pointer_y);
    panel_changed = 1;
}

namespace agos {

CODE_BANK(AGOS_VERB_BANK) void inventory_scroll(bool up) {
    if (up && inventory.line() == 0)
        return;
    show_icons(inventory.owner(),
        inventory.window(),
        static_cast<uint8_t>(up ? inventory.line() - 1 : inventory.line() + 1));
}

/// What a press comes to: the box under it names a verb and an item, and
/// subroutine 0 is the one whose lines are matched against them (0 then 100,
/// as handleVerbClicked runs them, verb.cpp:392-404).
CODE_BANK(AGOS_VERB_BANK) void command_run(uint16_t verb, uint16_t subject) {
    report::counts[report::CLICKED_VERB] = verb;
    vmstate::script.clicked(static_cast<int16_t>(verb), subject);
    report::count_one(report::CLICKS_RUN);
}

} // namespace agos

/// A press, and the pointer: only while the pointer shows, which is while
/// the game is waiting for the player (180 shows it, 181 hides it).
extern "C" CODE_BANK(AGOS_VERB_BANK) void click_banked() {
    report::count_one(report::CLICKED);
    report::counts[report::CLICKED_BOX] = boxes.at(clicked_x, clicked_y).id;
    if (cursor::visible())
        verb_bar.click(boxes, clicked_x, clicked_y);
}

extern "C" CODE_BANK(AGOS_VERB_BANK) void do_icons_banked() {
    show_icons(icons_item, icons_window, 0);
}

/// itemChildrenChanged (items.cpp:292): the icons showing that item redrawn
/// where they were scrolled to.
extern "C" CODE_BANK(AGOS_VERB_BANK) void items_changed_banked() {
    if (agos::changed_item != agos::NO_ITEM && agos::changed_item == inventory.owner())
        show_icons(inventory.owner(), inventory.window(), inventory.line());
}

/// CLS takes the icons in its window away with it (o_cls, script.cpp:642).
extern "C" CODE_BANK(AGOS_VERB_BANK) void clear_icons_banked() {
    if (inventory.window() == windows.current() && inventory.remove(boxes)) {
        verb_bar.box_gone(boxes, agos::HitAreas::ICON, pointer_y);
        panel_changed = 1;
    }
}

/// getDollar2's wait (FUN_0001b1ac): the world goes on, the pointer names
/// what it is over, and the first press on a box with an item is the answer.
/// A load asked for meanwhile unwinds the script instead, as it does a sync.
/// Time events wait: b1ac's idle (1b398) holds them while it runs.
extern "C" CODE_BANK(AGOS_VERB_BANK) void pick_object_banked() {
    verb_bar.ask(boxes);
    picked_item = agos::NO_ITEM;
    // A press made while the script ran is not an answer (b1ac clears its last
    // hit first; input.cpp:122-126).
    (void)mouse::clicked();
    while (!vmstate::script.returning()) {
        if (world_tick() == 0)
            continue;
        verb_bar.pointer(boxes, pointer_x, pointer_y);
        if (!mouse::clicked())
            continue;
        const mouse::Point at = mouse::where();
        const agos::HitAreas::Box box = verb_bar.pick(boxes, at.x, at.y);
        if (box.live()) {
            vmstate::script.set_variable(agos::CLICKED_TEXT_VARIABLE,
                static_cast<int16_t>(
                    (box.flags & agos::HitAreas::TEXT_BOX) != 0 ? box.text : agos::NOT_A_TEXT_BOX));
            picked_item = box.item;
            break;
        }
    }
    verb_bar.answered();
}

extern "C" CODE_BANK(AGOS_VERB_BANK) void pointer_banked() {
    if (cursor::visible())
        verb_bar.pointer(boxes, pointer_x, pointer_y);
}

/// What the script asked of the boxes, for the door below.
namespace {
agos::HitAreas::Box box_asked;
uint8_t box_opcode = 0;
bool box_alive = false;
} // namespace

/// ADD_BOX's packed thousands, or a conversation choice's slot, made into
/// the engine's flags (o_addBox, script.cpp:661; oww_addTextBox: TextBox and
/// BoxItem, the slot in the high byte).
extern "C" CODE_BANK(AGOS_VERB_BANK) void add_box_banked() {
    const uint8_t packed = box_asked.flags;
    if ((packed & agos::TEXT_CHOICE) != 0) {
        box_asked.text = static_cast<uint8_t>(packed & ~agos::TEXT_CHOICE);
        box_asked.flags = agos::HitAreas::TEXT_BOX | agos::HitAreas::BOX_ITEM;
    } else {
        box_asked.text = 0;
        box_asked.flags = agos::HitAreas::from_script(packed);
    }
    boxes.define(box_asked);
    report::counts[report::BOXES] = boxes.held();
    report::counts[report::BOXES_SPILLED] = boxes.spilled();
}

extern "C" CODE_BANK(AGOS_VERB_BANK) void box_banked() {
    const uint16_t id = box_asked.id;
    switch (box_opcode) {
        case agos::DEL_BOX:
            boxes.undefine(id);
            verb_bar.box_gone(boxes, id, pointer_y);
            break;
        case agos::ENABLE_BOX:
            (void)boxes.kill(id, false);
            break;
        case agos::DISABLE_BOX:
            if (boxes.kill(id, true))
                verb_bar.box_gone(boxes, id, pointer_y);
            break;
        case agos::MOVE_BOX:
            boxes.move(id, static_cast<int16_t>(box_asked.x), static_cast<int16_t>(box_asked.y));
            break;
        default: {
            const agos::HitAreas::Box box = boxes.with_id(id);
            box_alive = box.active();
            break;
        }
    }
}

/// The TABLES swap. In the store's bank because bring_in carries the same
/// transcoder store_open does, and a copy in the script bank would cost 2 KB
/// of it for a routine that runs at a room change.
namespace agos {

// In the dispatch's bank, as its only caller is; the boxes themselves are
// kept by the verb bar's bank, which is where the work is. Re-entered: a
// click in that bank runs the scripts that land here.
CODE_BANK(AGOS_SCRIPT_BANK)
void script_add_box(uint16_t id,
    uint16_t x,
    uint16_t y,
    uint16_t w,
    uint16_t h,
    uint8_t flags,
    uint16_t verb,
    uint16_t item) {
    box_asked = {id, x, y, w, h, verb, item, flags, 0};
    banked_reenter<add_box_banked>(AGOS_VERB_BANK);
}

CODE_BANK(AGOS_SCRIPT_BANK) void script_do_icons(uint16_t item, uint8_t window) {
    icons_item = item;
    icons_window = window;
    banked_reenter<do_icons_banked>(AGOS_VERB_BANK);
}

CODE_BANK(AGOS_SCRIPT_BANK) uint16_t script_pick_object() {
    banked_reenter<pick_object_banked>(AGOS_VERB_BANK);
    return picked_item;
}

CODE_BANK(AGOS_SCRIPT_BANK) uint16_t script_route_point(uint16_t x, uint16_t y) {
    return vmstate::animation.nearest_route_point(
        x, y, static_cast<uint16_t>(vmstate::script.variable(agos::PATH_ROUTE_VAR)));
}

CODE_BANK(AGOS_SCRIPT_BANK) bool script_box(uint16_t id, uint8_t opcode) {
    box_asked.id = id;
    box_opcode = opcode;
    [[clang::always_inline]] banked_reenter<box_banked>(AGOS_VERB_BANK);
    return box_alive;
}

void script_mouse_on() {
    cursor::shown(true);
}

void script_mouse_off() {
    cursor::shown(false);
    report::count_one(report::MOUSE_OFF);
}

void script_window(
    uint8_t which, uint8_t x, uint16_t y, uint8_t cells, uint8_t rows, uint8_t flags) {
    windows.define(which, x, y, cells, rows, flags);
}

void script_use_window(uint8_t which) {
    windows.use(which);
}

void script_clear_window() {
    banked_reenter<clear_icons_banked>(AGOS_VERB_BANK);
    clear_window();
}

void script_ink(uint8_t colour) {
    windows.ink(colour);
}

/// A line for the window in force, global or the room's own: a
/// conversation's choices are locals (getStringPtrByID, string.cpp:130-134).
/// Resolved here rather than between frames, as the sentence line is; a local
/// is a card read only when its file is not the one resident.
// In the dispatch's bank: two call sites would otherwise see LTO put it in
// the fixed region, 130 bytes.
CODE_BANK(AGOS_SCRIPT_BANK) void script_show_string(uint16_t string) {
    resolve_id = string;
    banked_call(AGOS_TICK_BANK, resolve_shown_banked);
    const uint8_t n = resolved;
    char* const line = speech::lent_line();
    report::count_one(report::SHOWN);
    report::counts[report::SHOWN_LEN] = n;
    report::counts[report::SHOWN_WINDOW] =
        static_cast<uint16_t>(windows.current() * 256 + windows.at(windows.current()).cells);
    if (n != 0) {
        say_in_window(line, n);
        panel_changed = 1;
    }
}

void script_fade_to_black() {
    banked_call(AGOS_EXTRA_BANK, fade_to_black_banked);
}

// In the dispatch's bank, beside their only call sites: from the fixed
// region they would cost chip, and the script bank's door is not this one.
// One door for both, so the re-entry is paid once.
CODE_BANK(AGOS_VGA_BANK) static void vga_box(uint16_t id, uint8_t opcode) {
    box_asked.id = id;
    box_opcode = opcode;
    [[clang::always_inline]] banked_reenter<box_banked>(AGOS_VERB_BANK);
}

CODE_BANK(AGOS_VGA_BANK) void vga_enable_box(uint16_t id) {
    vga_box(id, ENABLE_BOX);
}

CODE_BANK(AGOS_VGA_BANK) void vga_move_box(uint16_t id, int16_t dx, int16_t dy) {
    box_asked.x = static_cast<uint16_t>(dx);
    box_asked.y = static_cast<uint16_t>(dy);
    vga_box(id, MOVE_BOX);
}

void vga_mouse(bool shown) {
    if (shown) {
        script_mouse_on();
    } else {
        script_mouse_off();
        (void)mouse::clicked(); // a press waiting is let go, as the engine's is
    }
}

} // namespace agos

/// A zone's pixels, in the bank that holds the card path and the arena.
extern "C" CODE_BANK(AGOS_STORE_BANK) void store_pixels_banked() {
    agos::store.take_pixels();
}

extern "C" CODE_BANK(AGOS_STORE_BANK) void store_bring_in_banked() {
    agos::store.bring_in_wanted();
}

/// Where the voices are and the inventory's icons, once at start, in the
/// store's bank so the fixed region pays nothing for either. A card without
/// speech says nothing, and its lines hold for their text alone. One without
/// the icons cannot be played, so it stops here.
extern "C" CODE_BANK(AGOS_STORE_BANK) void side_files_banked() {
    agos::far_fill(atticmap::VOICE_INDEX, 0, static_cast<uint16_t>(atticmap::VOICE_INDEX_BYTES));
    (void)agos::read_game_file(
        atticmap::VOICE_INDEX_FILE, atticmap::VOICE_INDEX, atticmap::VOICE_INDEX_BYTES);
    talk::begin();
    if (agos::read_game_file(atticmap::ICONS_FILE, atticmap::ICONS, atticmap::ICONS_BYTES) == 0)
        fault(FAULT_NO_ICONS);
}

/// runSubroutine101, which is where the game starts itself. In a bank with the
/// store because it is the same startup and the same 1,750 bytes of dispatch
/// the fixed region has no room for.
extern "C" CODE_BANK(AGOS_STORE_BANK) void script_start_banked() {
    // The engine seeds a time event for subroutine 1 before it runs 101, and
    // the data never asks for it (addTimeEvent(0, 1), agos.cpp:1050). That
    // subroutine sets the game's own variables, its bits and the four boxes
    // the verb bar clicks; nothing later runs without them.
    vmstate::script.add_timeout(0, 1);
    vmstate::script.start(101);
}

/// The rows' prefixes, written once at start.
extern "C" CODE_BANK(AGOS_EXTRA_BANK) void prefixes_banked() {
    display_prefixes();
}

/// Setting the VIC up runs once and builds 25 rows of tokens, so it is worth
/// a bank rather than a permanent share of the region the code lives in.
extern "C" CODE_BANK(AGOS_DISPLAY_BANK) void display_begin_banked() {
    display_begin();
    banked_call(AGOS_EXTRA_BANK, prefixes_banked);
}

/// Dark and quiet before anything else, and before main.
///
/// The screen the ROM leaves sits at $0800, which is inside this program's own
/// low RAM: the moment the runtime initialises its data the text on screen
/// becomes whatever that data is. The banks load after that and take seconds.
/// A constructor is the earliest this program can speak, so it speaks here.
///
/// The audio-DMA channels go quiet in the same breath. A reset does not clear
/// them -- a program that left a channel running leaves it running -- and the
/// noise a freshly loaded machine makes is that channel, not this one.
[[gnu::constructor(101)]] static void dark_and_quiet() {
    VICIV.bordercol = 0;
    VICIV.screencol = 0;
    agos::blacken_palette();
    display_blank();
    tune_store_silence();
}

int main() {
    // The ROM may have left the CPU slow, and everything below assumes it did
    // not.
    // $0000 is the CPU port, not a null pointer (iomap.txt:2).
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    CPU_PORT_DDR = CPU_FORCE_40MHZ;

    arm_stack_guard();

    // Before the first thing that trusts Attic, which is the tune loader.
    if (!attic_works())
        fault(FAULT_NO_ATTIC);

    // Only once the Attic answers, since that is where this bank lives.
    banked_call(AGOS_EXTRA_BANK, extra_bank_hello);

    // The card, read by this program rather than by the hypervisor: a trap
    // dispatches no interrupt at all, and the interrupt is what plays the music.
    // Mounted first, because everything below reads its files through it.
    if (!agos::mount_card(GAME_DIRECTORY))
        fault(FAULT_NO_DIRECTORY);

    if (!tune_store_works())
        fault(FAULT_TUNES);

    // The game's own character sets, staged from its executable: one the
    // windows are drawn in, one an actor talks in. A card without them is a
    // card staged by an older tool, and saying so here is better than a screen
    // of blanks with nothing to explain it. Attic, because the CPU reads a font
    // and the VIC never does -- what the VIC reads is the glyphs these are
    // rendered into.
    if (!speech::begin())
        fault(FAULT_NO_FONT);

    banked_call(AGOS_STORE_BANK, side_files_banked);

    if (agos::read_game_file(text::WIN_FILE, atticmap::WINFONT, text::WIN_BYTES) !=
            text::WIN_BYTES ||
        agos::read_game_file(text::SAY_FILE, atticmap::SAYFONT, text::SAY_FONT_BYTES) !=
            text::SAY_FONT_BYTES)
        fault(FAULT_NO_FONT);

    // The picture and the panel start empty rather than holding whatever chip
    // RAM did. Nothing paints a backdrop until the script asks for one, and a
    // machine that has been running something else shows that something in
    // every glyph the display fetches.
    // One job: the backdrop is 43,520 bytes, inside a DMA length.
    static_assert(chipmap::BACKDROP_BYTES <= 0xFFFF, "the backdrop wants a loop");
    agos::far_fill(chipmap::BACKDROP, 0, static_cast<uint16_t>(chipmap::BACKDROP_BYTES));
    agos::far_fill(chipmap::PANEL, 0, chipmap::PANEL_BYTES);
    agos::far_fill(atticmap::PANEL_MASTER, 0, atticmap::PANEL_MASTER_BYTES);
    // The line trace disarmed: Attic keeps the last run's arm byte.
    agos::far_fill(atticmap::LINE_TRACE, 0, atticmap::LINE_TRACE_HEADER);

    // And so do the boxes. Chip RAM comes up holding whatever it held, and a
    // box store that reads it as boxes has no room for the real ones --
    // fourteen spilled and none defined -- which the host, whose memory is
    // wiped, never sees.
    boxes.clear();
    frame::begin();
    // Attic keeps what the last run left, so the ring's count is nonsense until
    // it is told otherwise -- and a reader cannot tell a stale record from a
    // fresh one.
    agos::far_write16_le(trace::AT, 0);
    // The shadow is in the region the linker only reserves, so it holds what the
    // last program left until something says otherwise.
    for (uint16_t i = 0; i < agos::SHADOW_BYTES; ++i)
        agos::shadow_palette[i] = 0;

    // Set up and left blank: there is nothing to show yet.
    banked_call(AGOS_DISPLAY_BANK, display_begin_banked);
    frame::check_composite();
    // Which tune plays, and when, is the script's (127 PLAY_TUNE); the driver
    // is not ticked until it has a module, and the channels were silenced
    // before any of this began.
    DMA.auden = DMA_AUDEN;

    IRQ_VECTOR = reinterpret_cast<uint16_t>(&irq_entry);
    VICII.irr = 0x0f; // drop anything pending before unmasking
    VICII.imr = VIC_IRQ_RASTER;
    asm volatile("cli" ::: "p");

    banked_call(AGOS_STORE_BANK, store_open_banked);

    // No room is painted here: the script controls the display with 96
    // PICTURE, which loads a zone, runs its image script and shows it. The
    // zone animations set their own palettes.
    //
    // The display is turned on with nothing in it: a glyph of nought paints
    // nothing, so until the script asks for a picture the screen is the border.
    room::show();

    // The pointer, which is the one hardware sprite: everything else that moves
    // is an RRB token. Over the whole screen, since the verbs and the inventory
    // are below the picture.
    mouse::begin(0,
        0,
        chipmap::CELLS_ACROSS * chipmap::CELL_LINES - 1,
        chipmap::SCREEN_ROWS * chipmap::CELL_LINES - 1);
    cursor::begin(1);
    // Hidden, as the engine hides it before it runs a line of script
    // (vc34_setMouseOff, agos.cpp:1066). 180 shows it when the player may act.
    cursor::shown(false);

    report::counts[report::MAGIC] = report::RUNNING;

    banked_call(AGOS_STORE_BANK, script_start_banked);

    // Game time counts from here, not from the frames start-up took.
    frames_taken = frames_now();
    for (;;) {
        if (const uint8_t periods = world_tick(); periods != 0) {
            const Stamp script_began = stamp();
            // What is under the pointer, lit and named, and a press turned into a
            // verb or a command: the verb bar's, in its bank with the boxes. The
            // pointer itself was read by the tick.
            banked_call(AGOS_VERB_BANK, pointer_banked);
            if (mouse::clicked()) {
                const mouse::Point at = mouse::where();
                clicked_x = at.x;
                clicked_y = at.y;
                banked_call(AGOS_VERB_BANK, click_banked);
            }

            // A script asks for a subroutine by leaving its id in variable 254, and
            // the engine runs it from its main loop and clears the variable
            // (hitarea_stuff_helper, input.cpp:368). Thirty-nine places in this
            // release's animation scripts ask that way, across seventeen zones.
            if (cutscene_ended != 0) {
                cutscene_ended = 0;
                talk::stop_voice();
                script_wanted = END_CUTSCENE_SUB;
                banked_call(AGOS_SCRIPT_BANK, run_wanted_banked);
            }
            if (const uint16_t wanted =
                    static_cast<uint16_t>(vmstate::script.variable(SCRIPT_ASKS));
                wanted != 0) {
                vmstate::script.set_variable(SCRIPT_ASKS, 0);
                script_wanted = wanted;
                banked_call(AGOS_SCRIPT_BANK, run_wanted_banked);
            }

            // The script's own clock, in seconds, which is what ADD_TIMEOUT counts
            // in. A subroutine due now runs here, between frames, never inside one.
            tick_of_second = static_cast<uint8_t>(tick_of_second + periods);
            if (tick_of_second >= TICKS_A_SECOND) {
                tick_of_second = static_cast<uint8_t>(tick_of_second - TICKS_A_SECOND);
                ++script_seconds;
                if (const uint8_t ran = vmstate::script.clock(script_seconds); ran != 0)
                    report::count_add(report::TIMEOUTS_RUN, ran);
                report::counts[report::TIMEOUTS_HELD] = vmstate::script.timeouts();
            }

            count_since(report::SCRIPT_LINES, script_began);
            report::counts[report::TABLES] = agos::store.tables_loaded();
            report::counts[report::ZONES] = agos::store.zones_loaded();
            report::counts[report::ZONES_FORCED] = agos::store.zones_forced();
        }
        saves::act();
    }
}
