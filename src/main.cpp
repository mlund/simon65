// Simon the Sorcerer on the MEGA65: the interrupt, and what runs under it.
//
// The only translation unit. Every module below is a header carrying its own
// definitions, so there is nothing to declare twice and LTO has nothing to
// stitch back together.
//
// The game's own script drives everything: it paints the room, animates the
// sprites over it and says what is said. What cannot be seen on the screen is
// read back out of report::counts instead.

#include "banks.hpp"
#include "chipmap.hpp"
#include "composite.hpp"
#include "cursor.hpp"
#include "diagnostics.hpp"
#include "display.hpp"
#include "figures.hpp"
#include "hitareas.hpp"
#include "inventory.hpp"
#include "localtext.hpp"
#include "masks.hpp"
#include "mouse.hpp"
#include "planar.hpp"
#include "savegame.hpp"
#include "sound.hpp"
#include "target_hooks.hpp"
#include "text.hpp"
#include "verbbar.hpp"
#include "vmstate.hpp"
#include "windows.hpp"

#include <mega65.h>

/// Frames since boot, counted by the raster interrupt. The music runs from that
/// interrupt so a card read cannot gap it; work that outlasts a frame is timed
/// against this, which a raster alone cannot do.
extern "C" volatile uint16_t frames = 0; // extern "C" so the monitor finds it

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

/// No zone asked for. Not nought, which is Simon's own zone.
constexpr uint8_t NO_ZONE_WANTED = 0xFF;

/// How many timed-out syncs the report keeps, which is all of them so far.
constexpr uint16_t SYNC_LOST_KEPT = 4;

/// The variable a script leaves a subroutine id in for the loop to run
/// (input.cpp:368).
constexpr uint8_t SCRIPT_ASKS = 254;

/// A sprite of the room's own, which spawns the others.

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

/// `frames`, read whole: it is two bytes the interrupt writes, so a read that
/// straddles a carry is read again.
[[nodiscard]] [[gnu::noinline]] uint16_t frames_now() {
    uint16_t was, now;
    do {
        was = frames;
        now = frames;
    } while (was != now);
    return now;
}

/// The zone whose figures the next banked load should fetch.
uint8_t zone_wanted = NO_ZONE_WANTED;

/// The figures decoded so far, and where each one is in the Attic arena.
///
/// In .bss, which is ram_high and has the room: the near part of the index is
/// a kilobyte. And the crt zeroes .bss, so what is here at boot is nothing,
/// not whatever the last program left in the reserved low memory.
agos::FigureCache figures;

/// What is on screen this frame, in the sprite list's order.
Placed layers[LAYERS_MAX];

/// Which figure each layer is, so the compositor knows an unchanged stack.
uint8_t layer_zone[LAYERS_MAX];
uint16_t layer_image[LAYERS_MAX];
static_assert(LAYERS_MAX <= composite::MAX_LAYERS, "the compositor masks layers in 32 bits");

/// The whole placement each sprite was last drawn with.
///
/// If a sprite's next cel is not decoded, keeping the old pose holds the
/// figure in place. Without it, walking figures blink and scenes show dropped
/// frames, worst where a scene has many cels in a hurry (the fireworks).
/// The engine never meets this: every image is in memory, so a sprite always
/// has something to draw.
///
/// The placement and not merely the cel. Cels of a walk differ in height and
/// the script picks a sprite's y for the cel it just chose, so holding last
/// frame's art at the new y shifts the figure's bottom edge. Holding all of it
/// makes a late cel a pose held for a frame, which is what standing still
/// looks like.
///
/// Direct-mapped on the sprite's own id, because the list compacts when a
/// sprite halts and a slot number means nothing across that. The glyph is
/// kept to notice a tenant that has moved in the pool since; a move costs
/// no more than a miss, so it is let be.
///
/// Thirty-two, not sixteen: a zone's cast is a run of consecutive ids and
/// zone 8's is 802 to 823, so at sixteen the second half collides with the first --
/// 818 on 802, 819 on 803 -- and each evicts the other's pose every tick,
/// causing the blinking. Thirty-two holds any cast this release has in one
/// scene without collisions.
constexpr uint8_t SHOWN_SLOTS = 32;
uint16_t shown_id[SHOWN_SLOTS];
uint16_t shown_image[SHOWN_SLOTS];
uint16_t shown_glyph[SHOWN_SLOTS];
Placed shown_at[SHOWN_SLOTS];

uint8_t layer_count = 0;

/// Bumped whenever the backdrop changes, so a mask's cut of it is cut again.
uint8_t backdrop_serial = 0;

/// Draws in a row that kept the last frame because a cel it needed was not
/// decoded, and the most there may be. The CD32 never shows part of a frame:
/// it decodes every cel on every draw into a hidden buffer, and a slow draw
/// is a late frame (runit2 0x22812, 0x1bd48). Holding is that late frame;
/// the cap is for a cel that never comes, which would freeze the picture.
uint8_t held_draws = 0;
constexpr uint8_t HOLD_MOST = 8;

/// Cels a drawn sprite will likely want next, for decoding in time nobody is
/// using. A sprite steps its image by one far more often than not: 74% were
/// the image before plus one, 86% within three. A ring, oldest overwritten:
/// a guess that waited too long is a guess about a past frame.
struct Ahead {
    uint8_t zone;
    uint16_t image;
};
constexpr uint8_t AHEAD = 3; //!< images past each drawn one
/// A power of two. A room changing many sprites at once, like the cart's,
/// would overwrite a smaller ring before it was decoded.
constexpr uint8_t AHEAD_RING = 32;
/// Out of zero page, where the allocator would put it and evict hotter
/// compositor variables: bank 3 grew by 108 bytes. Read only where written.
[[gnu::section(".noinit")]] Ahead ahead_ring[AHEAD_RING];
uint8_t ahead_put = 0, ahead_take = 0;

/// Guess at the cels after @p image of a sprite in @p zone.
[[gnu::always_inline]] inline void guess_ahead(uint8_t zone, uint16_t image) {
    for (uint8_t k = 1; k <= AHEAD; ++k) {
        ahead_ring[ahead_put & (AHEAD_RING - 1)] = {zone, static_cast<uint16_t>(image + k)};
        ++ahead_put;
    }
    if (static_cast<uint8_t>(ahead_put - ahead_take) > AHEAD_RING)
        ahead_take = static_cast<uint8_t>(ahead_put - AHEAD_RING);
}

/// One more layer for this frame, and which figure it is.
[[gnu::always_inline]] inline void lay(const Placed& layer, uint8_t zone, uint16_t image) {
    layer_zone[layer_count] = zone;
    layer_image[layer_count] = image;
    layers[layer_count++] = layer;
}

/// The physical raster line, read so the high bits cannot change between the
/// two bytes (iomap.txt:219-220, $D052 and $D053.0-2).
[[gnu::always_inline]] inline uint16_t raster_line() {
    uint8_t high, low;
    do {
        high = VICIV.fn_raster_msb & RASTER_MSB_BITS;
        low = VICIV.fn_raster_lsb;
    } while ((VICIV.fn_raster_msb & RASTER_MSB_BITS) != high);
    return static_cast<uint16_t>(high << 8 | low);
}

} // namespace
extern "C" [[gnu::used]] const agos::PackBits PACK_TABLE = agos::PACK;
namespace {

/// Physical raster lines in a frame (pixel_driver.vhdl:580).
constexpr uint16_t LINES_A_FRAME = 624;

/// Raster lines since @p line_began, @p frames_began frames ago: modulo a
/// frame, plus whole frames from the counter past the first, which may be one
/// out. For work that is usually shorter than a frame.
[[gnu::always_inline]] inline uint16_t lines_since(uint16_t frames_began, uint16_t line_began) {
    const auto crossed = static_cast<uint16_t>(frames_now() - frames_began);
    uint16_t lines = static_cast<uint16_t>(raster_line() + LINES_A_FRAME - line_began);
    if (lines >= LINES_A_FRAME)
        lines = static_cast<uint16_t>(lines - LINES_A_FRAME);
    if (crossed > 1)
        lines = static_cast<uint16_t>(lines + (crossed - 1) * LINES_A_FRAME);
    return lines;
}

/// Add @p lines to the running counter @p slot.
[[gnu::always_inline]] inline void count_lines(uint8_t slot, uint16_t lines) {
    report::counts[slot] = static_cast<uint16_t>(report::counts[slot] + lines);
}

/// When something started: the frame and the raster line.
struct Stamp {
    uint16_t frame;
    uint16_t line;
};

/// The time now, for timing what follows. Out of line, one copy for every
/// bank: inline at each site it cost the display bank 564 bytes. Four bytes,
/// so it comes back in registers.
[[nodiscard]] [[gnu::noinline]] Stamp stamp() {
    return {frames_now(), raster_line()};
}

/// Add the raster lines since @p began to the running counter @p slot.
[[gnu::noinline]] void count_since(uint8_t slot, Stamp began) {
    count_lines(slot, lines_since(began.frame, began.line));
}

/// This frame's input to the compositor, summed in the display bank.
uint32_t composite_input = 0;

/// Merges stacked sprites, in its own bank with all its working memory.
COMPOSITE_DATA composite::Compositor compositor;

/// Merge this frame's stacks; the door into the compositor's bank.
extern "C" COMPOSITE_BANKED void composite_banked() {
    layer_count =
        compositor.run(figures, layers, layer_count, layer_zone, layer_image, composite_input);
    if (compositor.merged > report::counts[report::COMPOSITES_PEAK])
        report::counts[report::COMPOSITES_PEAK] = compositor.merged;
    report::counts[report::COMPOSITE_BUILDS] = compositor.builds;
    report::counts[report::COMPOSITE_REPLAYS] = compositor.replays;
}

/// Whether DMA reaches the compositor's buffers, asked once at start-up.
extern "C" COMPOSITE_BANKED void composite_check_banked() {
    if (!compositor.reaches())
        agos::note_fault(agos::Fault::COMPOSITE_SCRATCH, 0);
}

/// The animation VM's rate against the frame's: 50 ms a tick over 20 ms a
/// frame is two ticks in five frames.
constexpr uint8_t VGA_FIFTHS_PER_FRAME = 2;
constexpr uint8_t FIFTHS_PER_VGA_TICK = 5;
uint8_t vga_fifths = 0;

/// Which rows of each display list carried a figure when that list was last
/// built, so a row its figures have left is put back.
uint8_t was_touched[rrb::LISTS][chipmap::SCREEN_ROWS] = {};

/// Rows [@p first, @p end) changed under every figure: both lists owe them.
void rows_owed(uint8_t first, uint8_t end) {
    for (uint8_t list = 0; list < rrb::LISTS; ++list)
        for (uint8_t row = first; row < end; ++row)
            was_touched[list][row] = 1;
}

/// The list the VIC shows, and one the draw has built and wants shown, plus
/// one; nought is none. The draw builds into the other list and the raster
/// interrupt swaps them, as the CD32 flips its bitmaps in vertical blank
/// (runit2 FUN_0001d566).
uint8_t shown_list = 0;
volatile uint8_t swap_to = 0;

/// Fast-forward, toggled: the game's clock runs as fast as the card and the
/// decoders allow. Every period still runs; none is skipped.
constexpr uint8_t HURRY_KEY = 'f';
constexpr uint8_t HURRY_PERIODS = 8;
volatile uint8_t hurry = 0;

/// Escape, which ends a cutscene the script has said may be ended. The
/// engine's own skip (_exitCutscene, input.cpp:574), not a debugging one.
constexpr uint8_t ESCAPE_KEY = 0x1B;
volatile uint8_t exit_cutscene = 0;

/// A key the tick took off the queue for the loop to act on.
uint8_t held_key = 0;
/// Whether the pointer was hidden when the held key was pressed: a save or
/// load key is refused then (save_load_key_banked), decided once so the
/// tick's unwind and the loop's load cannot disagree.
uint8_t key_refused = 0;

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

/// A line of the game's own font on screen, until 63 MESSAGE can ask for one.
/// Pixel-placed rather than on a glyph row, which is what the blank glyph at
/// either end of a text column is for.
/// Where the script says a line goes. Four places, as the engine has
/// (string.cpp:182): sprite ids 1, 2, 101 and 102.
struct SaidAt {
    int16_t x = 0;
    uint8_t y = 0;
    uint16_t width = 0;
};
constexpr uint8_t SAY_PLACES = 4;
SaidAt said_at[SAY_PLACES];

[[nodiscard]] uint8_t place_of(uint8_t which) {
    return which == 1 ? 0 : which == 2 ? 1 : which == 101 ? 2 : which == 102 ? 3 : SAY_PLACES;
}

/// A line's timer sprite is 199 + the speaker (printScreenText,
/// string.cpp:566): one per place a line can be said.
constexpr uint16_t TEXT_SPRITE_BASE = 199;
/// Whether @p id is a line's timer sprite, 200 to 199 + 255.
[[nodiscard]] bool is_text_sprite(uint16_t id) {
    const uint16_t which = static_cast<uint16_t>(id - TEXT_SPRITE_BASE);
    return id > TEXT_SPRITE_BASE && which <= UINT8_MAX &&
        place_of(static_cast<uint8_t>(which)) < SAY_PLACES;
}

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

/// The F key the tick took, for the loop to save or load by.
uint8_t slot_key = 0;

extern "C" void click_banked();
extern "C" void pointer_banked();
extern "C" void slot_key_banked();
extern "C" void save_game_banked();
extern "C" void load_game_banked();
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
extern "C" void save_load_key_banked();
extern "C" void load_check_banked();
extern "C" void kill_animate_banked();
extern "C" void show_room_banked();
extern "C" void prefixes_banked();

/// The text windows below the picture, and what is written in them.
agos::Windows windows;

/// The room's own strings. Globals are in gameamiga and the store has them.
agos::LocalText local_text;

/// A line the script has asked for, and the line resolved. Kept apart because
/// resolving reads the card, and the VM runs inside a frame.
uint16_t say_string = 0;
/// The line's voice, nought for none: spoken on channel 3 (speak_banked).
uint16_t say_speech = 0;
uint8_t say_which = 0, say_colour = 0, say_asked = 0;
char say_line[agos::LocalText::MOST_CHARS];
uint8_t say_len = 0;
uint16_t say_serial = 0;

/// Four slots on the F keys, slot 0 being the postcard's: F1 loads slot 0 and
/// F2, which is Shift+F1 on this keyboard, saves it; F3/F4 slot 1, to F7/F8.
/// F1 to F8 are $F1 to $F8 in ASCIIKEY (matrix_to_ascii.vhdl:91-94, :168-171),
/// so an odd key loads.
constexpr uint8_t FIRST_SLOT_KEY = 0xF1;
constexpr uint8_t LAST_SLOT_KEY = 0xF8;
[[nodiscard]] constexpr bool is_slot_key(uint8_t key) {
    return key >= FIRST_SLOT_KEY && key <= LAST_SLOT_KEY;
}
[[nodiscard]] constexpr bool is_load_key(uint8_t key) {
    return is_slot_key(key) && (key & 1) != 0;
}
[[nodiscard]] constexpr uint8_t slot_of(uint8_t key) {
    return static_cast<uint8_t>((key - FIRST_SLOT_KEY) >> 1);
}

/// What a save or load key did, said by the border for half a second: there
/// is no other way to see that the card was written. The border takes a
/// palette index and the game owns all 256, so the flash is whichever entry
/// of the room's palette comes nearest the colour wanted.
enum Flash : uint8_t { FLASH_FAILED, FLASH_SAVED, FLASH_LOADED };
constexpr uint8_t FULL = 63; // a palette component's top (0..63)

/// The palette entry nearest pure red, green or blue for @p flash, by summed
/// difference.
CODE_BANK(AGOS_SAVE_BANK) static uint8_t nearest_flash_colour(Flash flash) {
    uint8_t best = 0;
    uint16_t best_distance = UINT16_MAX;
    for (uint16_t entry = 0; entry < agos::PALETTE_ENTRIES; ++entry) {
        uint16_t distance = 0;
        for (uint8_t c = 0; c < 3; ++c) {
            const uint8_t have = agos::shadow_palette[entry * 3 + c];
            const uint8_t want = c == flash ? FULL : 0;
            distance = static_cast<uint16_t>(distance + (have > want ? have - want : want - have));
        }
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<uint8_t>(entry);
        }
    }
    return best;
}
constexpr uint16_t FLASH_FRAMES = 25;
/// The frame the flash ends on, or nought when there is none.
uint16_t flash_ends = 0;
/// Whether the last save or load key's work landed.
uint8_t key_worked = 0;

/// What the game's own load does after LOAD_USER_GAME, which the load key
/// copies: subroutine 141 sets bit 97, and 100, which runs after every click,
/// sees it and re-enters the player's room through subroutine 7 (gameamiga
/// subroutines 141 and 100).
constexpr uint16_t RELOADED_BIT = 97;
constexpr uint16_t RELOADED_MASK = 1U << (RELOADED_BIT % 16);
constexpr uint16_t AFTER_CLICK_SUB = 100;

/// Set while Simon idles (subroutine 160). The game's own save is a click,
/// and a click ends the idle first (subroutine 0 runs 161), so its saves never
/// hold the bit; one that does loads with no idle animation left to send the
/// sync 161 waits for, and stalls the next click for 50 s or more.
constexpr uint16_t IDLE_BIT = 70;
constexpr uint16_t IDLE_MASK = 1U << (IDLE_BIT % 16);

/// The animation VM runs at 20 Hz, so twenty of its ticks are a second --
/// which is the unit ADD_TIMEOUT counts in.
constexpr uint8_t TICKS_A_SECOND = 20;
uint8_t tick_of_second = 0;
uint32_t script_seconds = 0;

/// Lines of speech a pool slot holds: at three, a box the width of the
/// picture is 15 cells by 5 glyphs, which is 75 of the slot's 94.
constexpr uint8_t SAY_LINES = 3;
/// The palette block a line's sprite is drawn with (printScreenText,
/// string.cpp:568), and so its text's.
constexpr uint8_t TEXT_PALETTE = 12;

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
/// It holds what fires on a scene change, where 1.88x costs nothing. Nothing
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

extern "C" void load_zone_banked();

/// Whether anything has written to the panel since the rows were last built.
static uint8_t panel_changed = 0;

/// The window in force cleared, its cursor home.
static void clear_window() {
    windows.clear();
    panel_changed = 1;
}

/// A global string -- gameamiga's -- copied near. Locals come from LocalText;
/// these are indexed already (GameDb::string).
[[nodiscard]] static uint8_t copy_global_into(uint16_t id, char* into, uint8_t most) {
    const agos::Place from = agos::store.db().string(id);
    if (from == agos::NOWHERE)
        return 0;
    uint8_t n = 0;
    while (n + 1u < most) {
        const uint8_t ch = agos::far_read8(from + n);
        if (ch == 0)
            break;
        into[n++] = static_cast<char>(ch);
    }
    into[n] = '\0';
    return n;
}

/// A global string into the line of speech's buffer; its length.
[[nodiscard]] static uint8_t copy_global(uint16_t id) {
    return copy_global_into(id, say_line, sizeof say_line);
}

/// Any string, global or the room's own. One call site for the local copy,
/// which is a card read and nearly a kilobyte, so it is not inlined twice;
/// in the world's bank, where its first caller is and the fixed region is not.
[[gnu::noinline]] CODE_BANK(AGOS_TICK_BANK) static uint8_t
    resolve_string(uint16_t id, char* into, uint8_t most) {
    return id >= agos::LocalText::FIRST_LOCAL ? local_text.copy(id, into, most)
                                              : copy_global_into(id, into, most);
}

/// The same from another bank, into the sentence line's buffer: banked_call
/// takes no arguments, so the id goes in and the length comes back here.
static uint16_t resolve_id = 0;
static uint8_t resolved = 0;
/// One line of the six-pixel font across the screen: 53 characters.
constexpr uint8_t SENTENCE_CHARS = chipmap::CELLS_ACROSS * chipmap::CELL_LINES / text::WIN_ADVANCE;
static char name_line[SENTENCE_CHARS + 1];

extern "C" CODE_BANK(AGOS_TICK_BANK) void resolve_banked() {
    resolved = resolve_string(resolve_id, name_line, sizeof name_line);
}

/// The line an actor is saying, rendered into a pool slot and placed.
///
/// Wrapped at the width the script gave and centred in it, as the original
/// centres by padding each line with spaces before rendering (string.cpp:533).
/// Rendered once: the slot is keyed by the line, so a line that stands for a
/// hundred frames is drawn in one of them.
static void say_over_the_room() {
    const uint8_t place = place_of(say_which);
    const SaidAt& box = said_at[place < SAY_PLACES ? place : 0];
    const uint16_t wide = box.width != 0 ? box.width : chipmap::PICTURE_LINES;
    const uint8_t cells = text::cells_for(wide);

    // Three lines at most: a pool slot is 94 glyphs, and a wider box with more
    // lines than that is counted rather than drawn (TOO_WIDE).
    uint8_t lines = 0;
    uint8_t start[SAY_LINES] = {};
    uint8_t length[SAY_LINES] = {};
    for (uint8_t at = 0; at < say_len && lines < SAY_LINES;) {
        const uint8_t took = text::say_break(say_line + at, wide);
        if (took == 0)
            break;
        start[lines] = at;
        length[lines] = took;
        ++lines;
        at = static_cast<uint8_t>(at + took + 1); // past the space it broke at
    }
    if (lines == 0)
        return;

    const uint8_t rows = text::rows_for(lines);
    bool fresh = false;
    const agos::Figure fig = figures.text_slot(say_serial, cells, rows, &fresh);
    if (!fig.valid())
        return;
    if (fresh) {
        const agos::Place at = agos::Place{fig.glyph} * chipmap::GLYPH_BYTES;
        const uint8_t stride = static_cast<uint8_t>(rows + 2);
        text::clear(at, cells, stride);
        for (uint8_t i = 0; i < lines; ++i) {
            const uint16_t drawn = text::say_width(say_line + start[i], length[i]);
            const uint8_t left = static_cast<uint8_t>(drawn < wide ? (wide - drawn) / 2 : 0);
            // In the block the line's sprite is drawn with (string.cpp:568), not
            // the room's.
            text::render_say(say_line + start[i],
                length[i],
                text::say_ink(TEXT_PALETTE, say_colour),
                at,
                stride,
                cells,
                static_cast<uint8_t>(i * text::SAY_ROWS),
                left);
        }
    }
    const Placed line = placed(fig, static_cast<int16_t>(box.x / chipmap::CELL_LINES), box.y);
    if (line.cells != 0)
        layers[layer_count++] = line;
}

/// The priority whose masks restore only Simon's colours, 0x20 to 0x2F,
/// rather than everything under them (runit2 0x1e5b0): a dithered
/// see-through Simon in rooms 85-87. A layer cannot ask what is under it, so
/// these are not drawn.
constexpr uint16_t SEE_THROUGH_PRIORITY = 49;

/// A mask this port does not draw: the see-through kind.
[[nodiscard, gnu::always_inline]] static inline bool draws_nothing(const agos::VgaSprite& one) {
    return (one.flags & agos::DRAW_MASKED) != 0 && one.priority == SEE_THROUGH_PRIORITY;
}

/// Whether a sprite's new cel is still not decoded, once asked for: the draw
/// then leaves the screen as it is. Decided before any want(), so a held
/// frame takes nothing from the pool. Two sprites in step -- Simon's limbs
/// and body on the ladder -- otherwise show one without the other. In the
/// tick's chip bank: the display bank has no room, and this runs every frame.
static uint8_t cel_missing = 0;
extern "C" CODE_BANK(AGOS_VGA_TICK_BANK) void cels_missing_banked() {
    const agos::VgaSprite* sprite = vmstate::animation.sprites();
    const uint8_t count = vmstate::animation.sprite_count();
    cel_missing = 0;
    // Only what the draw would lay: it stops at LAYERS_MAX and leaves out a
    // sprite with nothing on the picture, and a cel nobody sees must not stop
    // the clock. Off the left or the top depends on a size not yet decoded, so
    // only the right edge is known here; the count is at most the draw's.
    uint8_t laid = 0;
    for (uint8_t k = 0; k < count && laid < LAYERS_MAX; ++k) {
        const agos::VgaSprite& one = sprite[k];
        if (one.image == 0 || one.x >= chipmap::CELLS_ACROSS || draws_nothing(one))
            continue;
        const agos::PackedZones::Pixels pixels = agos::zone_pixels.find(one.zone);
        if (!pixels.valid())
            continue; // a zone not loaded is the draw's to ask for
        ++laid;
        const uint8_t seen = static_cast<uint8_t>(one.id & (SHOWN_SLOTS - 1));
        if ((shown_id[seen] == one.id && shown_image[seen] == one.image) ||
            figures.ready(one.zone, one.image, one.palette))
            continue;
        if (!figures.ask(one.zone, one.image, pixels) ||
            !figures.ready(one.zone, one.image, one.palette))
            cel_missing = 1;
    }
}

/// What a mask's cut was cut for, and the key its pool slot is held under. A
/// new cut takes a new key, as a merge does (composite_slot), so a slot found
/// is always the cut wanted. Direct-mapped on the sprite id: a zone's masks
/// are consecutive ids, at most about nine live (zone 21). An unwritten entry
/// has id nought, which no sprite has.
namespace {
struct MaskCut {
    uint16_t id, image, key;
    int16_t x, y;
    uint8_t zone, serial;
};
} // namespace
constexpr uint8_t MASK_CUTS = 16;
EXTRA_DATA static MaskCut mask_cuts[MASK_CUTS];
EXTRA_DATA static uint16_t mask_keys;

/// Which sprite the draw hands the door below.
static uint8_t mask_index = 0;

static_assert(sizeof display_detail::screen_row >= masks::SCRATCH_BYTES,
    "a cut works in the row buffer, free while sprites are placed");

/// A masked sprite's layer: the backdrop cut to its image, laid where the
/// sprite is in the list. In the Attic: a cut is made once and then found,
/// and the frame that makes one is a scene change or a mask that moved.
extern "C" CODE_BANK(AGOS_EXTRA_BANK) void mask_banked() {
    const agos::VgaSprite& one = vmstate::animation.sprites()[mask_index];
    if (draws_nothing(one)) {
        report::counts[report::MASKS_SEE_THROUGH] =
            static_cast<uint16_t>(report::counts[report::MASKS_SEE_THROUGH] + 1);
        return;
    }
    // Decoded like any cel, asked for by cels_missing_banked; nothing is laid
    // until then.
    agos::Row art;
    if (!figures.decoded_row(one.zone, one.image, &art) || art.rows == 0)
        return;

    // Over the backdrop alone a cut changes nothing, and it costs every row it
    // covers: lay it only where something laid before it this frame lies under.
    const int16_t left = static_cast<int16_t>(one.x * chipmap::CELL_LINES);
    const int16_t right = static_cast<int16_t>(left + art.cells * 2 * chipmap::CELL_LINES);
    const int16_t bottom = static_cast<int16_t>(one.y + art.rows * chipmap::CELL_LINES);
    bool under = false;
    for (uint8_t k = 0; k < layer_count && !under; ++k) {
        const Placed& p = layers[k];
        const auto x0 = static_cast<int16_t>(p.at);
        const int16_t y0 = y_of(p);
        under = x0 < right && left < x0 + p.cells * cell_px(p.flags) && y0 < bottom &&
            one.y < y0 + p.rows * chipmap::CELL_LINES;
    }
    if (!under)
        return;

    MaskCut& was = mask_cuts[one.id & (MASK_CUTS - 1)];
    if (was.id != one.id || was.image != one.image || was.zone != one.zone || was.x != one.x ||
        was.y != one.y || was.serial != backdrop_serial)
        was = {one.id, one.image, ++mask_keys, one.x, one.y, one.zone, backdrop_serial};
    bool fresh = false;
    const agos::Figure fig =
        figures.mask_slot(was.key, static_cast<uint8_t>(art.cells * 2), art.rows, &fresh);
    if (!fig.valid())
        return;
    if (fresh)
        masks::cut({agos::arena_at(art.page),
                       agos::Place{fig.glyph} * chipmap::GLYPH_BYTES,
                       art.cells,
                       art.rows,
                       one.x,
                       one.y},
            chipmap::BACKDROP,
            masks::stand_in(agos::shadow_palette, agos::PALETTE_ENTRIES),
            display_detail::screen_row);
    // Full colour and transparent: a byte of nought lets the layers under it
    // show, and the backdrop's own nought became a colour in the cut.
    const Placed on = placed(fig, one.x, one.y);
    if (on.cells == 0)
        return;
    lay(on, agos::MASK_ZONE, was.key);
    report::counts[report::MASKS_LAID] =
        static_cast<uint16_t>(report::counts[report::MASKS_LAID] + 1);
}

/// One frame's worth of sprites: their figures into the pool the VIC reads,
/// then every row rebuilt with the layers that reach it.
///
/// The figures are decoded once and live in Attic; this moves the ones on
/// screen down by DMA, which is what makes a frame affordable at all.
extern "C" CODE_BANK(AGOS_DISPLAY_BANK) void draw_frame_banked() {
    const agos::VgaSprite* sprite = vmstate::animation.sprites();
    const uint8_t count = vmstate::animation.sprite_count();

    layer_count = 0;
    // The list built last is not on screen until the interrupt swaps it, and
    // this frame builds into the one it replaces -- and evicts what that names.
    if (swap_to != 0) {
        report::counts[report::SWAP_WAITS] =
            static_cast<uint16_t>(report::counts[report::SWAP_WAITS] + 1);
        while (swap_to != 0) {
        }
    }
    // Nothing this frame places, nor the list on screen, may be evicted by
    // something later in it.
    figures.begin_frame();

    // A new cel not decoded yet: leave the screen as it is (cels_missing_banked).
    banked_call(AGOS_VGA_TICK_BANK, cels_missing_banked);
    if (cel_missing != 0) {
        if (held_draws < HOLD_MOST) {
            ++held_draws;
            report::counts[report::HELD_DRAWS] =
                static_cast<uint16_t>(report::counts[report::HELD_DRAWS] + 1);
            return;
        }
        report::counts[report::HOLDS_GIVEN_UP] =
            static_cast<uint16_t>(report::counts[report::HOLDS_GIVEN_UP] + 1);
    }
    held_draws = 0;
    // What the frame is about to decide about each sprite, when a monitor has
    // asked for it. After the hold, which ends no census it began.
    census::begin();

    // What the room's script painted is in the backdrop, under every row, so
    // the display list carries only what the scripts animate.
    const Stamp sprites_began = stamp();
    uint8_t i = 0;
    for (; i < count && layer_count < LAYERS_MAX; ++i) {
        // A line's timer sprite shows the engine's rendered text, which here is
        // drawn as a line of its own (time_the_line).
        if (sprite[i].image == 0 || is_text_sprite(sprite[i].id)) {
            census::note(i, census::NO_IMAGE);
            continue; // a sprite with no frame set yet draws nothing
        }
        // A zone the store has never seen: asked for here and fetched between
        // frames, never inside one. The fetch is a card read and a whole-zone
        // decode, about eighty frames of it, and a draw that waited for it stood
        // still for that long -- with the pointer, the music's timing and every
        // other sprite waiting with it. The sprite is left out until its figures
        // arrive, which is a tick or two.
        //
        // One a frame, the first that asks: the next frame asks again for
        // whatever is still missing.
        const agos::PackedZones::Pixels pixels = agos::zone_pixels.find(sprite[i].zone);
        if (!pixels.valid()) {
            // A zone the card has not got is an answer too: ask for one it has not
            // been asked for, and let the rest alone.
            if (zone_wanted == NO_ZONE_WANTED && !agos::zone_pixels.knows(sprite[i].zone))
                zone_wanted = sprite[i].zone;
            report::counts[report::SKIPPED_ZONE] =
                static_cast<uint16_t>(report::counts[report::SKIPPED_ZONE] + 1);
            census::note(i, census::ZONE_ABSENT);
            continue;
        }
        // A mask draws nothing of its own: it puts the clean picture back over
        // what came before it (DRAW_MASKED).
        if ((sprite[i].flags & agos::DRAW_MASKED) != 0) {
            mask_index = i;
            banked_call(AGOS_EXTRA_BANK, mask_banked);
            continue;
        }
        const uint8_t seen = static_cast<uint8_t>(sprite[i].id & (SHOWN_SLOTS - 1));
        // A cel this sprite was not drawn with last tick: the animation asking,
        // before anything here has had a say in whether it can be answered.
        const bool new_cel = shown_id[seen] != sprite[i].id || shown_image[seen] != sprite[i].image;
        if (new_cel)
            report::counts[report::CELS_WANTED] =
                static_cast<uint16_t>(report::counts[report::CELS_WANTED] + 1);
        const agos::Figure fig =
            figures.want(sprite[i].zone, sprite[i].image, sprite[i].palette, pixels);
        if (!fig.valid()) {
            // The pose it held last frame, if the pool still has that cel where it
            // had it. Laid as it was laid then, position and all.
            if (shown_id[seen] == sprite[i].id &&
                figures.held(sprite[i].zone, shown_image[seen], sprite[i].palette).glyph ==
                    shown_glyph[seen] &&
                layer_count < LAYERS_MAX) {
                census::note(i, census::DRAWN);
                report::counts[report::CELS_HELD] =
                    static_cast<uint16_t>(report::counts[report::CELS_HELD] + 1);
                lay(shown_at[seen], sprite[i].zone, shown_image[seen]);
                continue;
            }
            census::note(i, census::NO_FIGURE);
            report::counts[report::NOT_DRAWN] =
                static_cast<uint16_t>(report::counts[report::NOT_DRAWN] + 1);
            continue;
        }
        census::note(i, census::DRAWN);
        // A sprite's x is in eight-pixel units and its y in pixels:
        // xoffs = (vlut[0] * 2 + state->x) * 8, yoffs = vlut[1] + state->y
        // (gfx.cpp:940), and window 4 -- the room -- sits at 0, 0
        // (initialVideoWindows_Simon, agos.cpp:723). The script's own steps say
        // the same: SET_SPRITE_XY moves by one, which is eight pixels.
        const Placed on = placed(fig,
            sprite[i].x,
            sprite[i].y,
            static_cast<uint8_t>(rrb::FOUR_BIT |
                ((sprite[i].flags & agos::DRAW_FLIP) != 0 ? rrb::FLIP_HORIZONTAL : 0u)),
            rrb::four_bit_colour(sprite[i].palette));
        if (on.cells == 0)
            continue; // wholly outside the picture; the slot is worth more
        shown_id[seen] = sprite[i].id;
        shown_image[seen] = sprite[i].image;
        shown_glyph[seen] = fig.glyph;
        shown_at[seen] = on;
        lay(on, sprite[i].zone, sprite[i].image);
        // On a new cel only: a sprite holding a pose would refill the ring with
        // the same guesses every draw, each then refused by a lookup.
        if (new_cel)
            guess_ahead(sprite[i].zone, sprite[i].image);
    }

    // Stacks into one layer apiece, before the line of speech: text is never
    // merged, and it goes on top. One layer is no stack.
    if (layer_count > 1) {
        // Timed from here, bank switch and all: the compositor's bank has no room.
        const Stamp began = stamp();
        composite_input = composite::checksum(layers, layer_count, layer_zone, layer_image);
        banked_call(AGOS_COMPOSITE_BANK, composite_banked);
        count_since(report::COMPOSITE_LINES, began);
        report::counts[report::COMPOSITE_CALLS] =
            static_cast<uint16_t>(report::counts[report::COMPOSITE_CALLS] + 1);
    }

    count_since(report::SPRITE_LINES, sprites_began);

    // Sprites the frame never reached, because it had no layers left for them.
    if (i < count)
        report::counts[report::LAYERS_CAPPED] =
            static_cast<uint16_t>(report::counts[report::LAYERS_CAPPED] + 1);

    // The line's text is its timer sprite's image in the engine (string.cpp:
    // 545-556), so it goes when that sprite does: at its end, or a KILL_ANIMATE
    // on a room change. Searched apart, as the loop above can stop short.
    bool line_live = false;
    for (uint8_t k = 0; say_len != 0 && k < count; ++k)
        if (sprite[k].id == TEXT_SPRITE_BASE + say_which)
            line_live = true;
    if (say_len != 0 && line_live && layer_count < LAYERS_MAX)
        say_over_the_room();

    // The list as it stood, now every entry's fate is known and every layer is
    // laid -- a line of speech is one, so counting before it disagreed with
    // report::LAYERS for no reason but call order. A sprite past the last one
    // examined stays UNSEEN, which is what a full display list looks like from
    // outside.
    census::end(sprite, count, vmstate::animation.ticks(), layer_count, zone_wanted);

    report::counts[report::LAYERS] = layer_count;
    if (layer_count > report::counts[report::LAYERS_PEAK])
        report::counts[report::LAYERS_PEAK] = layer_count;
    report::counts[report::RESIDENT] = figures.held();
    report::counts[report::TOO_WIDE] = figures.too_wide();
    // Why a figure is not on screen, which is otherwise a thing only eyes can
    // report: too big for a zone's whole glyph run, refused by the decoder, or
    // simply not decoded yet.
    report::counts[report::TOO_BIG] = figures.too_big();
    report::counts[report::UNDECODED] = figures.undecoded();
    report::counts[report::DECODED] = figures.decoded();
    report::counts[report::ARENA_WRAPS] = figures.wraps();
    report::counts[report::AHEAD_DECODED] = figures.ahead_decoded();
    report::counts[report::AHEAD_USED] = figures.ahead_used();
    report::counts[report::AHEAD_ABANDONED] = figures.ahead_abandoned();
    report::counts[report::HINT_MISS] = figures.hint_miss();
    report::counts[report::DECODES_GIVEN_UP] = figures.given_up();
    report::counts[report::CROWDED] = figures.crowded();
    // What the display list cost, and whether any row could not hold it.
    report::counts[report::ROW_PEAK] = row_peak;
    report::counts[report::LAYERS_DROPPED] = layers_dropped;
    report::counts[report::CLOSES_FAILED] = closes_failed;

    // The rows below the picture, when a window has written to them.
    if (panel_changed != 0) {
        panel_changed = 0;
        rows_owed(chipmap::PICTURE_ROWS, chipmap::SCREEN_ROWS);
    }

    // Only the rows a figure touches, this frame or when the back list was last
    // built, two frames ago. The backdrop's own rows never change, and
    // rebuilding all twenty-five cost two frames where the figures cover about
    // ten.
    uint8_t touched[chipmap::SCREEN_ROWS] = {};
    for (uint8_t i = 0; i < layer_count; ++i)
        for (uint8_t row = layers[i].top < 0 ? 0 : uint8_t(layers[i].top);
            int{row} < layers[i].top + layers[i].rows && row < chipmap::SCREEN_ROWS;
            ++row)
            touched[row] = 1;

    const Stamp rows_began = stamp();
    const uint8_t back = static_cast<uint8_t>(shown_list ^ 1);
    for (uint8_t row = 0; row < chipmap::SCREEN_ROWS; ++row) {
        if (!touched[row] && !was_touched[back][row])
            continue;
        display_row_over(back, row, layers, layer_count);
        was_touched[back][row] = touched[row];
        report::counts[report::ROWS_BUILT] =
            static_cast<uint16_t>(report::counts[report::ROWS_BUILT] + 1);
    }
    count_since(report::ROWS_LINES, rows_began);
    figures.list_built();
    shown_list = back;
    swap_to = static_cast<uint8_t>(back + 1);
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
/// tick's bank because that bank had 362 bytes in it.
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
    ++backdrop_serial;
    agos::far_fill(chipmap::BACKDROP, 0, chipmap::PICTURE_BYTES);
    rows_owed(0, chipmap::PICTURE_ROWS);
}

/// A paint that did not happen, counted wherever it fell over: the whole
/// point of the census beside it is to tell a draw that was dropped from one
/// the scripts never made.
static void missed_a_paint() {
    report::counts[report::PAINT_MISSED] =
        static_cast<uint16_t>(report::counts[report::PAINT_MISSED] + 1);
}

/// One image painted where the script says, into the backdrop either way.
///
/// A room is not one picture: zone 64's script paints twenty-two, the base and
/// then its scenery, each over what is already there. They all belong in the
/// backdrop's glyphs. Keeping one painted figure instead lost every piece but
/// the last -- the room stood bare, and the hearth went black for the ten
/// ticks its fire animation leaves blank, where DRAW 9 had painted a burning
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
/// SET_WINDOW_IMAGE's half of the work that is not the script: the window's
/// picture is replaced, so the ground goes. In the room's bank because that
/// is where the picture's memory is handled.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void window_image_banked() {
    clear_the_picture();
}

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
    ++backdrop_serial;
    // Timed against the interrupt: a decode outlasts a raster by far.
    const uint16_t began = frames;
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
    report::counts[report::DECODE_FRAMES] = static_cast<uint16_t>(frames - began);
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

/// Where the next lines are said (161), and a line to say (162).
///
/// The line is not resolved here: a string can be a card read and the VM runs
/// inside a frame, so the loop picks it up once the frame is done with.
static void agos_text_box(uint8_t which, int16_t x, uint8_t y, uint16_t width) {
    const uint8_t place = place_of(which);
    if (place < SAY_PLACES) {
        said_at[place].x = x;
        said_at[place].y = y;
        said_at[place].width = width;
    }
}

static void agos_text_msg(uint8_t which, uint8_t colour, uint16_t string) {
    report::counts[report::SAID] = static_cast<uint16_t>(report::counts[report::SAID] + 1);
    report::counts[report::SAID_STRING] = string;
    say_which = which;
    say_colour = colour;
    say_string = string;
    say_asked = 1;
}

/// The voice's half of a line: its id, kept for time_the_voice (playSpeech,
/// res_snd.cpp:55). A talkie line is often voice alone -- string 0xFFFF --
/// and then the timer sprite is the only thing that sends sync 200: zone 2's
/// 201 + the speaker loops while the voice plays (IF_SPEECH) and then sends
/// it.
static void agos_speech(uint16_t speech) {
    say_speech = speech;
}

extern "C" CODE_BANK(AGOS_SCRIPT_BANK) void slot_key_banked() {
    if (is_slot_key(slot_key))
        banked_call(AGOS_SAVE_BANK, save_load_key_banked); // Attic: cold
}

// What a script asks the animation VM for, in the room's bank rather than
// the dispatch's: the picture path alone is the zone loader, the image
// script and the show, and bank 4 has 188 opcodes in it already. The
// dispatch reaches these through a door, as it reaches everything else that
// is not its own.
namespace {

uint8_t asked_zone = 0, asked_window = 0, asked_palette = 0;
uint16_t asked_image = 0, asked_sprite = 0, asked_ident = 0;
int16_t asked_x = 0, asked_y = 0;
bool asked_halt = false, asked_beard = false;

/// The verb bar's window, y 136 to 199 (SET_SUB_WINDOW in zone 0's image
/// script 0; vc26_setSubWindow, vga.cpp:1073).
constexpr uint8_t PANEL_WINDOW = 5;

} // namespace

extern "C" CODE_BANK(AGOS_ROOM_BANK) void load_asked_zone_banked() {
    agos::note_opcode(report::ZONES_ASKED, asked_zone);
    if (!agos::zone_pixels.knows(asked_zone)) {
        // What a zone costs off the card, which is the one thing in a tick that
        // is not this program's own work: a deleted directory entry goes on being
        // walked, and took a load from 292 ms to 1,242.
        const uint16_t began = frames;
        zone_wanted = asked_zone;
        banked_call(AGOS_STORE_BANK, load_zone_banked);
        zone_wanted = NO_ZONE_WANTED;
        const uint16_t took = static_cast<uint16_t>(frames - began);
        report::counts[report::LOADS] = static_cast<uint16_t>(report::counts[report::LOADS] + 1);
        if (took > report::counts[report::WORST_LOAD])
            report::counts[report::WORST_LOAD] = took;
    }
}

extern "C" CODE_BANK(AGOS_ROOM_BANK) void picture_banked() {
    load_asked_zone_banked();
    if (!agos::zone_pixels.find(asked_zone).valid())
        return; // the card had nothing; SKIPPED_ZONE says so
    vmstate::animation.use_window(asked_window);
    vmstate::animation.run_image(asked_zone, asked_image);
    show_room_banked();
    report::counts[report::PICTURES] = static_cast<uint16_t>(report::counts[report::PICTURES] + 1);
    // The panel's picture owes only its own rows, which its paints did.
    if (asked_window != PANEL_WINDOW)
        rows_owed(0, chipmap::SCREEN_ROWS);
}

/// As much of one image as this frame has left, in the bank the decoders live
/// in: the draw asks for it the first time something wants that image.
///
/// The budget is the frame itself rather than a count. A piece is about 1.7
/// ms, so decoding stops within the frame's end and never overshoots by more;
/// atomic decoding of a large figure takes eight frames of stalling.
/// The fewest pieces a slice does, whatever the clock says, and the most.
///
/// The ceiling is not about speed: the floor and the frame test both read
/// `frames`, which only the raster interrupt advances, so a slice that
/// trusted the clock alone would spin for ever if the interrupt ever stalled
/// -- a stall became a wedged machine, with `frames` frozen at 220.
constexpr uint8_t PIECES_LEAST = 24;
constexpr uint8_t PIECES_MOST = 96;

/// One piece of the figure in flight. Out of line so both the slice below and
/// the decode-ahead share the one copy of the decoder the step inlines.
[[gnu::noinline]] CODE_BANK(AGOS_ROOM_BANK) static void step_once() {
    figures.decode_step();
}

/// Take a figure on, demanded or guessed. Out of line for the same reason as
/// step_once: one copy of the start, the image table and the arena included.
[[gnu::noinline]] CODE_BANK(AGOS_ROOM_BANK) static void start_decode(
    uint8_t zone, uint16_t image, agos::PackedZones::Pixels from, bool ahead) {
    figures.decode_start(zone, image, from, ahead);
}

/// The decoder's doors for the decode-ahead, which runs in the world's bank:
/// one piece, and a guessed figure started from what decode_zone and its
/// fellows hold.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void step_banked() {
    step_once();
}
extern "C" CODE_BANK(AGOS_ROOM_BANK) void start_ahead_banked() {
    start_decode(agos::decode_zone, agos::decode_wanted, agos::decode_from, true);
}

extern "C" CODE_BANK(AGOS_ROOM_BANK) void figures_decode_banked() {
    const Stamp began = stamp();
    start_decode(agos::decode_zone, agos::decode_wanted, agos::decode_from, false);
    // A floor as well as a ceiling. The draw has usually spent the frame by the
    // time it gets here, so "what is left" is often one piece -- and at one a
    // frame a title cel would take ninety-six of them. 24 and 96 are the knee
    // of the measured curve: from 8/32 to 128/240 the cel rate stayed flat, so
    // the slice was never what limited it.
    uint8_t pieces = 0;
    do {
        if (!figures.decoding())
            break;
        step_once();
        ++pieces;
    } while (pieces < PIECES_LEAST || (frames == began.frame && pieces < PIECES_MOST));
    // What a slice costs. One frame or less is the whole point of slicing.
    const uint16_t took = static_cast<uint16_t>(frames - began.frame);
    count_lines(report::DECODE_PIECES, pieces);
    count_since(report::SLICE_LINES, began);
    if (took > report::counts[report::WORST_DECODE])
        report::counts[report::WORST_DECODE] = took;
}

extern "C" CODE_BANK(AGOS_ROOM_BANK) void animate_banked() {
    load_asked_zone_banked();
    vmstate::animation.animate(
        asked_window, asked_zone, asked_sprite, asked_x, asked_y, asked_palette);
}

/// Beside the tick it stops.
extern "C" CODE_BANK(AGOS_VGA_TICK_BANK) void halt_animation_banked() {
    vmstate::animation.halt(asked_halt);
}

/// The beard on or off, in the store's bank: cold, and the store's to do.
extern "C" CODE_BANK(AGOS_STORE_BANK) void beard_banked() {
    if (agos::store.wear_beard(asked_beard))
        figures.forget_zone(agos::BEARD_ZONE);
}

/// The tune PLAY_TUNE asked for, for the door below.
static uint8_t asked_tune = 0;

/// Which tune each Attic slot holds, plus one so nought is none. A tune
/// stays until its slot is wanted, so a room the player goes back to finds
/// its music without the card.
EXTRA_DATA static uint8_t slot_tune[atticmap::TUNE_SLOTS];
EXTRA_DATA static uint8_t next_slot, playing_slot;

/// PLAY_TUNE's tune from its slot, read off the card into the next slot if
/// none holds it. Never into the playing one, whose patterns the interrupt
/// is reading. A tune that will not load is counted, and the old one plays on.
extern "C" CODE_BANK(AGOS_EXTRA_BANK) void play_tune_banked() {
    const auto want = static_cast<uint8_t>(asked_tune + 1);
    uint8_t slot = 0;
    while (slot < atticmap::TUNE_SLOTS && slot_tune[slot] != want)
        ++slot;
    if (slot == atticmap::TUNE_SLOTS) {
        slot = next_slot;
        if (slot == playing_slot && tune_loaded)
            slot = static_cast<uint8_t>((slot + 1) % atticmap::TUNE_SLOTS);
        next_slot = static_cast<uint8_t>((slot + 1) % atticmap::TUNE_SLOTS);
        slot_tune[slot] = 0;
        const agos::Place base = atticmap::slot(atticmap::TUNES, slot);
        char name[] = "TUNE00.BIN"; // the game's number
        constexpr uint8_t TENS = 4, UNITS = 5;
        for (uint8_t n = asked_tune; n != 0; --n)
            if (++name[UNITS] > '9') {
                name[UNITS] = '0';
                ++name[TENS];
            }
        if (agos::read_game_file(name, base, 1UL << atticmap::SHIFT) == 0 || !module_landed(base)) {
            report::counts[report::TUNE_MISSING] =
                static_cast<uint16_t>(report::counts[report::TUNE_MISSING] + 1);
            return;
        }
        slot_tune[slot] = want;
    }
    playing_slot = slot;
    tune_store_select(atticmap::slot(atticmap::TUNES, slot));
}

extern "C" CODE_BANK(AGOS_ROOM_BANK) void kill_animate_banked() {
    // Not drawn now: the engine's screen keeps the killed sprites until its
    // next tick, and a skipped cutscene (2928: kill, load, PICTURE) fades them
    // with the room. A sprite in palette block 13 or above stays lit through
    // that fade there too, since a room's fade takes 208 entries.
    vmstate::animation.reset_sprites();
}

/// One sprite stopped, through the animation VM's own vc60 (stopAnimate,
/// script.cpp:1058). The one place the stop is written: a second call site
/// moves it out of line into the fixed region, 332 bytes.
extern "C" CODE_BANK(AGOS_TICK_BANK) void stop_animate_banked() {
    vmstate::animation.stop(asked_sprite);
}

extern "C" CODE_BANK(AGOS_ROOM_BANK) void sync_banked() {
    report::counts[report::SYNC_SENT] = asked_ident;
    vmstate::animation.send_sync(asked_ident);
}

/// One turn of the world: the animation VM, the screen, and what the frame
/// asked to be fetched. False when this raster was not one of the 50 ms ticks.
///
/// A script waiting on a sync runs this too. Ticking only the animation VM
/// there left the screen on the frame the wait began with -- the intro played
/// out behind a still picture -- and ran it at processor speed besides, so the
/// whole of it went by in seconds. The engine's wait runs its own event loop
/// for both reasons (waitForSync, script.cpp:1117).
///
/// What stays in the main loop is everything that can start a script: the
/// mouse, the keys, the timeout clock. Inside a wait, the script they would
/// start is the one waiting.
/// In the display's bank, with the drawing it spends its time on: the fixed
/// region is full, and this is the one caller of the frame that is not.
/// banked_call takes a function of no arguments, so the answer comes back in
/// `ticked`: the periods this turn ran, for the script's clock.
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
/// symptom is a layout accident: adding 256 bytes to .bss turned a clean run
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

/// Decode the guessed cels while the frame lasts: what is in flight first,
/// then the next guess not decoded already. Stops the moment a frame passes,
/// so it never holds up a period; a piece is about half a millisecond. Here,
/// in the world's bank, because it only schedules: the work is through the
/// decoder's doors.
CODE_BANK(AGOS_TICK_BANK) static void decode_ahead() {
    const uint16_t began = frames_now();
    while (frames_now() == began) {
        if (figures.decoding()) {
            banked_call(AGOS_ROOM_BANK, step_banked);
            continue;
        }
        if (ahead_take == ahead_put)
            return;
        const Ahead next = ahead_ring[ahead_take & (AHEAD_RING - 1)];
        ++ahead_take;
        if (!figures.worth_ahead(next.zone, next.image))
            continue;
        const agos::PackedZones::Pixels from = agos::zone_pixels.find(next.zone);
        if (!from.valid())
            continue;
        agos::decode_zone = next.zone;
        agos::decode_wanted = next.image;
        agos::decode_from = from;
        banked_call(AGOS_ROOM_BANK, start_ahead_banked);
    }
}

extern "C" void animate_banked();

/// What a line of text starts besides its pixels (printScreenText,
/// string.cpp:495-499 and 560-568). The engine animates sprite 199 + the
/// speaker; its script in zone 2 waits variable 85 ticks and then sends
/// sync 200, which is what a script's wait for a line ends on. Without it
/// every line holds the game for the wait's whole thousand ticks.
constexpr uint8_t TALK_RATE_VARIABLE = 141; // ticks a three letters, talkie
constexpr uint8_t TALK_TICKS_VARIABLE = 85;
constexpr int16_t TALK_RATE_DEFAULT = 9;
constexpr uint16_t WIDE_TEXT_BIT = 133; // window 4 rather than 3
constexpr int16_t TEXT_TOP_LEAST = 2;

/// One of a line's sprites started afresh: the last one with its number
/// goes first (string.cpp:546; playSpeech's stopAnimate).
CODE_BANK(AGOS_TICK_BANK) static void start_line_sprite(uint16_t sprite, uint8_t window) {
    asked_sprite = sprite;
    asked_zone = static_cast<uint8_t>(sprite / agos::SPRITES_PER_ZONE);
    asked_window = window;
    stop_animate_banked(); // same bank: one call site keeps it here
    banked_call(AGOS_ROOM_BANK, animate_banked);
}

/// A voice off the card: its head before it plays, the rest a frame at a
/// time behind it. The card is far faster than the voice -- 43 sectors a
/// second at 22,050 Hz -- so the head only has to cover the frames until the
/// pump's next turn, and a zone load's stall.
namespace {
namespace talk {
constexpr uint16_t HEAD_SECTORS = 16; // 8 KB, 186 ms
constexpr uint16_t PUMP_SECTORS = 8;  // a frame's share
constexpr uint16_t SECTOR = fat32::SECTOR_BYTES;
static_assert(HEAD_SECTORS * SECTOR / sound::PAGE >= sound::RING_PAGES,
    "the head primes the ring whole, so none of it starts as silence");

uint16_t voice = 0;       // asked for by time_the_voice
uint32_t next_sector = 0; // in SPEECH.BIN
agos::Place next_into = 0;
uint16_t sectors_left = 0;
uint16_t pages_left = 0; // of the voice, not yet handed over
uint16_t began = 0;      // the frame it was asked for

uint16_t effect = 0; // asked for by PLAY_EFFECT
constexpr uint8_t NO_SET = 0xFF;
constexpr uint8_t SETS = 100;  // two digits in SETnn.BIN
uint16_t sound_set = 0;        // asked for by opcode 185
uint8_t sound_set_in = NO_SET; // whose effects are in the Attic

/// SPEECH.BIN, mapped once at start.
card::Runs<agos::DoorCard> runs;

/// Read @p n sectors on, and hand the refill the pages they hold.
SOUND_BANKED bool fetch(uint16_t n) {
    if (!runs.read(next_sector, next_into, n))
        return false;
    next_sector += n;
    next_into += agos::Place{n} * SECTOR;
    sectors_left = static_cast<uint16_t>(sectors_left - n);
    return true;
}

/// Pages of the voice in @p n sectors just read: two a sector, but the last
/// sector's padding is not voice.
SOUND_BANKED uint16_t pages_in(uint16_t n) {
    const uint16_t pages = static_cast<uint16_t>(n * (SECTOR / sound::PAGE));
    const uint16_t given = pages < pages_left ? pages : pages_left;
    pages_left = static_cast<uint16_t>(pages_left - given);
    return given;
}
} // namespace talk
} // namespace

/// Channel 3 set up, and SPEECH.BIN mapped for reading from anywhere: no
/// speech if it is missing or too scattered, which SPEECH_RUNS reads as nought.
extern "C" SOUND_BANKED void speech_map_banked() {
    sound::begin();
    report::counts[report::SPEECH_RUNS] =
        talk::runs.map(atticmap::SPEECH_FILE, atticmap::SPEECH_RUNS, atticmap::SPEECH_RUNS_MOST);
}

/// FADE_TO_BLACK: waits out eight frames, so in the Attic with what fires
/// on a scene change.
extern "C" CODE_BANK(AGOS_EXTRA_BANK) void fade_to_black_banked() {
    agos::fade_to_black();
}

/// SETnn.BIN for talk::sound_set, unless it is the one in already. A set
/// that will not read leaves effects silent. What is playing of the old one
/// stops first: its bytes are about to be written over.
extern "C" SOUND_BANKED void sound_set_banked() {
    if (talk::sound_set == talk::sound_set_in)
        return;
    sound::stop_effect();
    talk::sound_set_in = talk::NO_SET;
    if (talk::sound_set >= talk::SETS)
        return;
    char name[] = "SET00.BIN";
    name[3] = static_cast<char>('0' + talk::sound_set / 10);
    name[4] = static_cast<char>('0' + talk::sound_set % 10);
    // The door clears card_name before it returns, which the analyzer cannot
    // see through banked_call.
    // NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)
    if (agos::read_game_file(name, atticmap::EFFECTS, atticmap::EFFECTS_BYTES) != 0)
        talk::sound_set_in = talk::sound_set;
    // NOLINTEND(clang-analyzer-core.StackAddressEscape)
}

/// Start talk::effect from the set in: the effect playing stops, as a new
/// effect cuts the old on the CD32, and speech plays on.
extern "C" SOUND_BANKED void effect_banked() {
    if (talk::sound_set_in == talk::NO_SET || talk::effect >= atticmap::EFFECT_IDS)
        return;
    uint16_t entry[2]; // first page, pages
    agos::far_read(atticmap::EFFECTS + agos::Place{talk::effect} * sizeof entry,
        reinterpret_cast<uint8_t*>(entry),
        sizeof entry);
    if (entry[1] != 0)
        sound::effect(atticmap::EFFECTS + agos::Place{entry[0]} * sound::PAGE, entry[1]);
}

/// The voice cut, as a skipped cutscene cuts it (endCutscene,
/// subroutine.cpp:253-262): an effect plays on.
extern "C" SOUND_BANKED void voice_stop_banked() {
    sound::hush();
    talk::sectors_left = 0;
}

/// STOP_ALL_SOUNDS: both streams, and the speech's reading.
extern "C" SOUND_BANKED void sounds_stop_banked() {
    sound::stop();
    talk::sectors_left = 0;
}

/// Start talk::voice: the speech that was playing stops, as a new voice cuts
/// the old on the CD32 (runit2 0x1cd4a), and an effect plays on. A voice not
/// on the card is silence.
extern "C" SOUND_BANKED void speak_banked() {
    sound::hush();
    talk::sectors_left = 0;
    if (talk::voice >= atticmap::VOICES)
        return;
    uint32_t entry[2]; // first sector, bytes
    agos::far_read(atticmap::VOICE_INDEX + agos::Place{talk::voice} * sizeof entry,
        reinterpret_cast<uint8_t*>(entry),
        sizeof entry);
    if (entry[1] == 0)
        return;
    report::counts[report::VOICE_SPOKEN] = talk::voice;
    talk::began = frames_now();
    talk::next_sector = entry[0];
    talk::next_into = atticmap::SOUND;
    talk::sectors_left = static_cast<uint16_t>((entry[1] + talk::SECTOR - 1) / talk::SECTOR);
    talk::pages_left = static_cast<uint16_t>((entry[1] + sound::PAGE - 1) / sound::PAGE);
    const uint16_t head =
        talk::sectors_left < talk::HEAD_SECTORS ? talk::sectors_left : talk::HEAD_SECTORS;
    if (!talk::fetch(head)) {
        talk::sectors_left = 0;
        return;
    }
    sound::speak(atticmap::SOUND, talk::pages_in(head), talk::sectors_left != 0);
}

/// One frame's share of the voice off the card.
extern "C" SOUND_BANKED void speech_pump_banked() {
    const Stamp began = stamp();
    const uint16_t n =
        talk::sectors_left < talk::PUMP_SECTORS ? talk::sectors_left : talk::PUMP_SECTORS;
    if (talk::fetch(n)) {
        sound::arrived(static_cast<uint8_t>(talk::pages_in(n)));
    } else {
        talk::sectors_left = 0; // a card error: play what landed
    }
    const uint16_t took = lines_since(began.frame, began.line);
    count_lines(report::PUMP_LINES, took);
    if (took > report::counts[report::WORST_PUMP])
        report::counts[report::WORST_PUMP] = took;
    if (talk::sectors_left == 0) {
        sound::all_arrived();
        report::counts[report::VOICE_LOAD_FRAMES] =
            static_cast<uint16_t>(frames_now() - talk::began);
    }
}

/// The voice's timer: window 4 at nought, nought, speakers under 100 only.
constexpr uint16_t VOICE_SPRITE_BASE = 201;
constexpr uint8_t VOICE_WINDOW = 4, VOICED_SPEAKERS = 100;

CODE_BANK(AGOS_TICK_BANK) static void time_the_voice(uint8_t which, uint16_t speech) {
    // Ids past the table are cues, not voices (9999: no voice at all), and
    // only a speaker under 100 has a timer (playSpeech, res_snd.cpp:72-78).
    if (which >= VOICED_SPEAKERS || speech >= atticmap::VOICES)
        return;
    asked_x = 0;
    asked_y = 0;
    asked_palette = 0;
    start_line_sprite(static_cast<uint16_t>(VOICE_SPRITE_BASE + which), VOICE_WINDOW);
}

CODE_BANK(AGOS_TICK_BANK) static void time_the_line(uint8_t which, uint8_t length) {
    int16_t rate = vmstate::script.variable(TALK_RATE_VARIABLE);
    if (rate == 0) {
        rate = TALK_RATE_DEFAULT;
        vmstate::script.set_variable(TALK_RATE_VARIABLE, rate);
    }
    vmstate::script.set_variable(
        TALK_TICKS_VARIABLE, static_cast<int16_t>(rate * ((length + 3) / 3)));
    const uint8_t place = place_of(which);
    const SaidAt at = place < SAY_PLACES ? said_at[place] : SaidAt{};
    asked_x = static_cast<int16_t>(at.x / chipmap::CELL_LINES);
    asked_y = at.y < TEXT_TOP_LEAST ? TEXT_TOP_LEAST : at.y;
    asked_palette = TEXT_PALETTE;
    start_line_sprite(static_cast<uint16_t>(TEXT_SPRITE_BASE + which),
        vmstate::script.bit(WIDE_TEXT_BIT) ? agos::ROOM_WINDOW : agos::TEXT_WINDOW);
}

extern "C" CODE_BANK(AGOS_TICK_BANK) void world_tick_banked() {
    ticked = 0;
    const uint16_t now = frames_now();
    auto elapsed = static_cast<uint16_t>(now - frames_taken);
    if (elapsed == 0)
        return;
    frames_taken = now;
    if (talk::sectors_left != 0)
        banked_call(AGOS_SOUND_BANK, speech_pump_banked);
    // A save or load key's border flash, over.
    if (flash_ends != 0 && static_cast<int16_t>(now - flash_ends) >= 0) {
        VICIV.bordercol = 0;
        flash_ends = 0;
    }
    // The pointer moves whatever the script is doing: a wait on an animation
    // ticks the world here and nothing else, and the engine's pointer kept
    // moving through those. A press is latched for the loop, not acted on.
    const mouse::Point pointer = mouse::poll();
    cursor::at(pointer.x, pointer.y);
    pointer_x = pointer.x;
    pointer_y = pointer.y;
    if (elapsed > CATCH_UP_FRAMES) {
        report::counts[report::LOST_FRAMES] = static_cast<uint16_t>(
            report::counts[report::LOST_FRAMES] + (elapsed - CATCH_UP_FRAMES));
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
            key_refused = cursor::visible() ? 0 : 1;
            if (is_load_key(key) && key_refused == 0) {
                slot_key = key;
                banked_call(AGOS_SAVE_BANK, load_check_banked);
                if (key_refused == 0) {
                    vmstate::script.set_vga_wait_for(0);
                    vmstate::script.return_from_script();
                }
            }
            held_key = key;
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
    if (held_draws != 0) {
        report::counts[report::LOST_FRAMES] =
            static_cast<uint16_t>(report::counts[report::LOST_FRAMES] + elapsed);
    } else {
        vga_fifths = static_cast<uint8_t>(vga_fifths + VGA_FIFTHS_PER_FRAME * elapsed);
        while (vga_fifths >= FIFTHS_PER_VGA_TICK) {
            vga_fifths = static_cast<uint8_t>(vga_fifths - FIFTHS_PER_VGA_TICK);
            ++periods;
        }
        if (hurry != 0)
            periods = HURRY_PERIODS;
    }
    if (periods == 0 && held_draws == 0) {
        // A turn with no period due is the idle time decode-ahead is for.
        decode_ahead();
        return;
    }

    // Twice a period, and three times on every other one: that is what the
    // engine's timer does (timerProc, event.cpp:693-699), so a period is worth
    // two and a half passes of the animation VM. Ticking it once ran the game
    // at two fifths of its speed, which is what a player sees as half.
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
    const uint16_t vm_took = static_cast<uint16_t>(frames - vm_began.frame);
    report::counts[report::TICK_RASTERS] = vm_took;
    if (vm_took > report::counts[report::WORST_TICK])
        report::counts[report::WORST_TICK] = vm_took;

    const Stamp before = stamp();
    // A door between them now. They shared a bank to avoid one, but the display
    // bank holds the drawing and the row compositor and has no room for the
    // tick as well, and this is twenty crossings a second against a bank that
    // has to stay in chip RAM. Checked by eye: no tearing.
    banked_call(AGOS_DISPLAY_BANK, draw_frame_banked);
    // A decode in flight is finished by the frames, not by whichever sprite
    // happens to miss next: it is started from want(), and once every drawn
    // figure is resident nothing would step it again -- it would hold its
    // arena pages, unnamed by any row, until some image missed the hash.
    // Through the door: the draw is in the display bank and the decoder is in
    // the room bank, and a direct call would run whatever sits at that address
    // in this one.
    if (figures.decoding()) { // the start inside it is a no-op here
        const Stamp after_began = stamp();
        banked_call(AGOS_ROOM_BANK, figures_decode_banked);
        count_since(report::AFTER_DRAW_LINES, after_began);
    }
    count_since(report::DRAW_LINES, before);
    const uint16_t took = static_cast<uint16_t>(frames - before.frame);
    report::counts[report::FRAME_RASTERS] = took;
    // The peak, not just the last: one draw in a hundred is the one that
    // stalls, and a mean hides it.
    if (took > report::counts[report::WORST_FRAME])
        report::counts[report::WORST_FRAME] = took;
    report::counts[report::TICKS] = static_cast<uint16_t>(report::counts[report::TICKS] + 1);

    // The palette the scripts loaded, put up once the frame is built and not
    // while a fade is holding the screen black (displayScreen, draw.cpp:964).
    if (agos::palette_dirty != 0 && agos::palette_held == 0)
        agos::show_palette();

    // The line the script asked for, resolved now that the frame is done with:
    // a local string is a card read, which blocks the frame.
    if (say_asked != 0) {
        say_asked = 0;
        say_len = resolve_string(say_string, say_line, sizeof say_line);
        report::counts[report::SAID_LEN] = say_len;
        // The voice first, as the engine plays it before it prints: said at
        // every place, timed by a sprite at a speaker's.
        if (say_speech != 0) {
            talk::voice = say_speech;
            banked_call(AGOS_SOUND_BANK, speak_banked);
            time_the_voice(say_which, say_speech);
        }
        if (say_len != 0)
            time_the_line(say_which, say_len);
        ++say_serial; // a new line wants a slot of its own
        rows_owed(0, chipmap::SCREEN_ROWS);
    }

    // What the draw asked for, fetched now that the frame is done with. A zone
    // that will not read is marked absent by the loader, so this asks the card
    // once rather than once a frame for good.
    if (zone_wanted != NO_ZONE_WANTED) {
        banked_call(AGOS_STORE_BANK, load_zone_banked);
        zone_wanted = NO_ZONE_WANTED;
    }
    // Fast-forward reports one, as it always did: the script's clock runs at
    // the frame rate there, not eight times it.
    ticked = hurry != 0 && periods != 0 ? 1 : periods;
    // And what is left of this frame, after the draw.
    decode_ahead();
}

/// The turn of the world, through the door: how many periods it ran, nought
/// if none were due.
[[nodiscard]] static uint8_t world_tick() {
    banked_call(AGOS_TICK_BANK, world_tick_banked);
    return ticked;
}

/// Wait for the animation VM to say it has got there.
///
/// The engine spins its own event loop here (waitForSync) and so does this:
/// the animation VM is ticked until the rendezvous clears. Bounded, because a
/// sync that never comes would otherwise be a machine that has stopped with
/// no way to say why -- the counter says instead.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void wait_sync_banked() {
    report::counts[report::SYNC_WANTED] = asked_ident;
    // A sync that has already gone past counts as arrived (waitForSync,
    // script.cpp:1069). The scripts of this release sync before the main script
    // asks, and waiting for one already sent is a wait that never ends.
    if (asked_ident != agos::SPEECH_SYNC) {
        const uint16_t sent = vmstate::script.last_sync();
        vmstate::script.set_last_sync(0);
        if (sent == asked_ident)
            return;
    }
    vmstate::script.set_vga_wait_for(asked_ident);
    for (uint16_t spun = 0; spun < SYNC_MOST_TICKS;) {
        if (vmstate::script.vga_wait_for() == 0)
            return;
        // Escape, where the engine takes it: a wait is where a cutscene may be
        // skipped, and the bit is the script saying this one may. The flag
        // unwinds the script that was waiting, and subroutine 170, the game's
        // own ending for it, follows from the loop (endCutscene,
        // subroutine.cpp:253-262; see cutscene_ended).
        if (exit_cutscene != 0) {
            exit_cutscene = 0;
            if (vmstate::script.bit(CUTSCENE_BIT)) {
                vmstate::script.set_vga_wait_for(0);
                vmstate::script.return_from_script();
                cutscene_ended = 1;
                return;
            }
        }
        if (world_tick())
            ++spun;
    }
    vmstate::script.set_vga_wait_for(0);
    // Which one, not just how many: the ident names the sprite that never got
    // there, and the scene the script then ran past without.
    const uint16_t lost = report::counts[report::SYNC_GAVE_UP];
    if (lost < SYNC_LOST_KEPT)
        report::counts[report::SYNC_LOST + lost] = asked_ident;
    if (lost == 0) {
        report::counts[report::LOST_SAY_WHICH] = say_which;
        report::counts[report::LOST_SAY_SPEECH] = say_speech;
    }
    report::counts[report::SYNC_GAVE_UP] = static_cast<uint16_t>(lost + 1);
}

namespace agos {

void script_picture(uint8_t zone, uint16_t image, uint8_t window) {
    asked_zone = zone;
    asked_image = image;
    asked_window = window;
    banked_call(AGOS_ROOM_BANK, picture_banked);
}

void script_load_zone(uint8_t zone) {
    asked_zone = zone;
    banked_call(AGOS_ROOM_BANK, load_asked_zone_banked);
}

void script_animate(
    uint16_t window, uint8_t zone, uint16_t sprite, int16_t x, int16_t y, uint8_t palette) {
    asked_window = window;
    asked_zone = zone;
    asked_sprite = sprite;
    asked_x = x;
    asked_y = y;
    asked_palette = palette;
    banked_call(AGOS_ROOM_BANK, animate_banked);
}

void script_kill_animate() {
    banked_call(AGOS_ROOM_BANK, kill_animate_banked);
}

// In the dispatch's bank, its only caller: no fixed-region bytes for it.
CODE_BANK(AGOS_SCRIPT_BANK) void script_halt_animation(bool halted) {
    asked_halt = halted;
    banked_call(AGOS_VGA_TICK_BANK, halt_animation_banked);
}

// In the dispatch's bank, its only caller. Re-entered: the tick runs while
// the walk's frame is live.
CODE_BANK(AGOS_SCRIPT_BANK) void script_rescan() {
    banked_reenter<world_tick_banked>(AGOS_TICK_BANK);
}

// In the dispatch's bank, its only caller.
CODE_BANK(AGOS_SCRIPT_BANK) void script_beard(bool on) {
    asked_beard = on;
    banked_call(AGOS_STORE_BANK, beard_banked);
}

void script_stop_animate(uint16_t sprite) {
    asked_sprite = sprite;
    banked_call(AGOS_TICK_BANK, stop_animate_banked);
}

void script_save_game() {
    banked_call(AGOS_SAVE_BANK, save_game_banked);
}
void script_load_game() {
    banked_call(AGOS_SAVE_BANK, load_game_banked);
}

void script_sync(uint16_t ident) {
    asked_ident = ident;
    banked_call(AGOS_ROOM_BANK, sync_banked);
}

void script_wait_sync(uint16_t ident) {
    asked_ident = ident;
    banked_call(AGOS_ROOM_BANK, wait_sync_banked);
}

/// The last tune asked for, plus one so nought is none (the engine starts at
/// -1, agos.cpp:926). Not saved, as the engine does not save it either.
static uint8_t last_tune = 0;

/// A tune unless it is the one last asked for: the engine lets a room ask
/// again without restarting it (o_playTune, script.cpp:787). In the
/// dispatch's bank, its only caller.
CODE_BANK(AGOS_SCRIPT_BANK) void script_play_tune(uint16_t music, uint16_t /*track*/) {
    report::counts[report::TUNE_WANTED] = music;
    // The release's tunes number thirty-four, so a byte holds one.
    const auto asked = static_cast<uint8_t>(music + 1);
    if (asked == last_tune)
        return;
    last_tune = asked;
    asked_tune = static_cast<uint8_t>(music);
    banked_call(AGOS_EXTRA_BANK, play_tune_banked);
}

} // namespace agos

/// What a press comes to: the box under it names a verb and an item, and
/// subroutine 0 is the one whose lines are matched against them (0 then 100,
/// as handleVerbClicked runs them, verb.cpp:392-404).
/// The subroutine a script asked for by leaving its id in variable 254.
static uint16_t script_wanted = 0;

extern "C" CODE_BANK(AGOS_SCRIPT_BANK) void run_wanted_banked() {
    // A skip leaves the unwinding flag up; the subroutine it asks for must not
    // inherit it.
    vmstate::script.clear_returning();
    vmstate::script.start(script_wanted);
    vmstate::script.clear_returning();
}

/// The save files' names on the card, a slot each (genSaveName,
/// saveload.cpp:89); slot 0 is the postcard's. In the card bank, which is what
/// reads them, so the fixed region pays nothing.
constexpr uint8_t SAVE_SLOTS = 4;
constexpr uint8_t SAVE_NAME_BYTES = sizeof "SIMON1.000";
RODATA_BANK(AGOS_CARD_BANK)
static const char SAVE_FILE[SAVE_SLOTS][SAVE_NAME_BYTES] = {
    "SIMON1.000", "SIMON1.001", "SIMON1.002", "SIMON1.003"};
static_assert((LAST_SLOT_KEY - FIRST_SLOT_KEY + 1) / 2 == SAVE_SLOTS);
/// Each slot's name, for an index without a multiply.
SAVE_CONST static const char* const SAVE_NAME[SAVE_SLOTS] = {
    SAVE_FILE[0], SAVE_FILE[1], SAVE_FILE[2], SAVE_FILE[3]};

/// The slot the next save or load uses: an F key's, else the postcard's.
SAVE_DATA static uint8_t save_slot;

/// The image, worked on near while the save bank is mapped.
SAVE_DATA static uint8_t save_image[agos::SAVE_IMAGE_BYTES];

/// Where the next sector of a save comes from while the card walks, and the
/// image's length and sum, kept here rather than in locals because the walk
/// calls back into this bank (banks.hpp, banked_reenter).
SAVE_DATA static agos::Place save_from;
SAVE_DATA static uint16_t save_bytes, save_sum;

/// Where DMA finds the image (in_bank).
[[nodiscard, gnu::always_inline]] static agos::Place save_image_place() {
    return in_bank(BANK_BASE_OF(AGOS_SAVE_BANK), save_image);
}

/// Fletcher's two sums, mod 256: enough to tell a sector that did not land
/// from one that did, without a second 3.5 KB buffer to compare against.
CODE_BANK(AGOS_SAVE_BANK) static uint16_t image_sum(uint16_t bytes) {
    uint8_t low = 0, high = 0;
    for (uint16_t i = 0; i < bytes; ++i) {
        low = static_cast<uint8_t>(low + save_image[i]);
        high = static_cast<uint8_t>(high + low);
    }
    return static_cast<uint16_t>(high << 8 | low);
}

/// One sector of a save, asked for by the card reader's walk while
/// card_writing is set.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void card_store_banked() {
    if (!card::store_sector(agos::card_sector, save_from))
        agos::card_writing = 0;
    save_from += fat32::SECTOR_BYTES;
}

/// The game's state to its slot's file, which is staged at its full size so that
/// writing it changes nothing in the FAT. The walk lands the card's read-back
/// over the source, and that is summed against the image: a save that did not
/// land is a fault now rather than a lost game later.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void save_game_banked() {
    save_bytes = agos::SaveGame::save(vmstate::script, save_image, sizeof save_image);
    if (save_bytes == 0)
        return; // SAVE_TOO_LONG, raised by the codec
    save_sum = image_sum(save_bytes);
    agos::far_copy(save_image_place(), atticmap::SAVEGAME, save_bytes);
    agos::far_fill(atticmap::SAVEGAME + save_bytes,
        0,
        static_cast<uint16_t>(atticmap::SAVEGAME_BYTES - save_bytes));
    save_from = atticmap::SAVEGAME;
    agos::card_writing = 1;
    const uint32_t file =
        agos::read_game_file(SAVE_NAME[save_slot], atticmap::SAVEGAME, atticmap::SAVEGAME_BYTES);
    const bool wrote = agos::card_writing != 0;
    agos::card_writing = 0;
    if (wrote && file >= save_bytes) {
        agos::far_copy(atticmap::SAVEGAME, save_image_place(), save_bytes);
        if (image_sum(save_bytes) == save_sum) {
            key_worked = 1;
            return;
        }
    }
    agos::script_fault(agos::Fault::SAVE_NO_FILE, save_bytes);
}

/// The slot's file into save_image: how many bytes, or nought with the
/// fault raised when it is not on the card.
CODE_BANK(AGOS_SAVE_BANK) static uint16_t read_save() {
    const uint32_t bytes =
        agos::read_game_file(SAVE_NAME[save_slot], atticmap::SAVEGAME, atticmap::SAVEGAME_BYTES);
    if (bytes == 0) {
        agos::script_fault(agos::Fault::SAVE_NO_FILE, 0);
        return 0;
    }
    // The file's padding past the largest image is never read.
    const uint16_t held =
        bytes < sizeof save_image ? static_cast<uint16_t>(bytes) : sizeof save_image;
    agos::far_copy(atticmap::SAVEGAME, save_image_place(), held);
    return held;
}

/// The slot, plus one, whose image load_check_banked left checked in
/// save_image, and its length: the load that follows takes it from there
/// rather than off the card again. Nought is none.
SAVE_DATA static uint8_t checked_slot;
SAVE_DATA static uint16_t checked_held;

/// The slot's file into the game. False, with the fault raised, when it is not on
/// the card or is not this game's.
CODE_BANK(AGOS_SAVE_BANK) static bool load_game() {
    const uint16_t held = checked_slot == save_slot + 1 ? checked_held : read_save();
    checked_slot = 0;
    return held != 0 && agos::SaveGame::load(vmstate::script, save_image, held);
}

/// Whether load key slot_key's slot would load, asked before the tick
/// unwinds the running script for it: a blank slot refuses the key, and the
/// script runs on. Refusal is key_refused, which the border then shows.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void load_check_banked() {
    save_slot = slot_of(slot_key);
    checked_held = read_save();
    if (checked_held != 0 && agos::SaveGame::check(vmstate::script, save_image, checked_held))
        checked_slot = static_cast<uint8_t>(save_slot + 1);
    else
        key_refused = 1;
    save_slot = 0;
}

/// LOAD_USER_GAME, inside a script: the script that asked rebuilds the scene
/// itself (gameamiga subroutine 141), as it does in the engine.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void load_game_banked() {
    (void)load_game();
}

/// The load key, from the main loop with no script running, so nothing is
/// left to rebuild the scene. It does what 141 does after its load that this
/// interpreter has: bit 97 for subroutine 100 to see -- asked for through
/// variable 254, so the loop runs it next tick, from the top, rather than
/// this door starting a script inside itself.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void load_key_banked() {
    if (!load_game())
        return;
    key_worked = 1;
    // Whatever scene was playing goes with its script, through KILL_ANIMATE's
    // own door: the saved room's script puts back what belongs.
    banked_call(AGOS_ROOM_BANK, kill_animate_banked);
    // By word, not set_bit: that is the fixed region's, and a second caller in
    // another bank would cost it a copy.
    vmstate::script.bits()[RELOADED_BIT / 16] |= RELOADED_MASK;
    vmstate::script.set_variable(SCRIPT_ASKS, AFTER_CLICK_SUB);
    // And what the engine draws itself after a load (saveload.cpp:162).
    icons_item = agos::PLAYER;
    icons_window = INVENTORY_WINDOW;
    banked_call(AGOS_VERB_BANK, do_icons_banked);
}

/// The save and load keys, and the border saying how each went.
///
/// Refused while the pointer is hidden, as ScummVM refuses its quick save and
/// load (quickLoadOrSave, saveload.cpp:127-140): the intro, cutscenes and
/// conversations, where the game itself offers neither. A load there skips
/// what the intro's end sets up -- the panel, the sentence ink, the first
/// room's palettes -- and Escape gets past it. A save is refused too while
/// Simon idles (IDLE_BIT).
extern "C" CODE_BANK(AGOS_SAVE_BANK) void save_load_key_banked() {
    key_worked = 0;
    const bool load = is_load_key(slot_key);
    if (!load && (vmstate::script.bits()[IDLE_BIT / 16] & IDLE_MASK) != 0)
        key_refused = 1;
    if (key_refused == 0) {
        save_slot = slot_of(slot_key);
        if (load)
            load_key_banked();
        else
            save_game_banked();
        save_slot = 0;
    }
    VICIV.bordercol = nearest_flash_colour(key_worked == 0 ? FLASH_FAILED
            : load                                         ? FLASH_LOADED
                                                           : FLASH_SAVED);
    flash_ends = static_cast<uint16_t>(frames_now() + FLASH_FRAMES);
    if (flash_ends == 0)
        flash_ends = 1; // nought means no flash
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
// Its own buffer: say_line belongs to the line of speech over the room,
// which may be showing.

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
        n = copy_global_into(item.object().name(), name_line, sizeof name_line);
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
    asked_zone = ARROWS_ZONE;
    banked_call(AGOS_ROOM_BANK, load_asked_zone_banked);
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

CODE_BANK(AGOS_VERB_BANK) void command_run(uint16_t verb, uint16_t subject) {
    report::counts[report::CLICKED_VERB] = verb;
    vmstate::script.clicked(static_cast<int16_t>(verb), subject);
    report::counts[report::CLICKS_RUN] =
        static_cast<uint16_t>(report::counts[report::CLICKS_RUN] + 1);
}

} // namespace agos

/// A press, and the pointer: only while the pointer shows, which is while
/// the game is waiting for the player (180 shows it, 181 hides it).
extern "C" CODE_BANK(AGOS_VERB_BANK) void click_banked() {
    report::counts[report::CLICKED] = static_cast<uint16_t>(report::counts[report::CLICKED] + 1);
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
    report::counts[report::MOUSE_OFF] =
        static_cast<uint16_t>(report::counts[report::MOUSE_OFF] + 1);
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

/// A line for the window in force. Resolved here rather than between frames:
/// a window line is a global string, which is already in Attic and costs no
/// card read -- unlike an actor's, which can be a local.
// In the dispatch's bank: two call sites would otherwise see LTO put it in
// the fixed region, 130 bytes.
CODE_BANK(AGOS_SCRIPT_BANK) void script_show_string(uint16_t string) {
    const uint8_t n = copy_global(string);
    report::counts[report::SHOWN] = static_cast<uint16_t>(report::counts[report::SHOWN] + 1);
    report::counts[report::SHOWN_LEN] = n;
    report::counts[report::SHOWN_WINDOW] =
        static_cast<uint16_t>(windows.current() * 256 + windows.at(windows.current()).cells);
    if (n != 0) {
        say_in_window(say_line, n);
        panel_changed = 1;
    }
}

void script_text_box(uint8_t which, int16_t x, uint8_t y, uint16_t width) {
    agos_text_box(which, x, y, width);
}

void script_text_msg(uint8_t which, uint8_t colour, uint16_t string, uint16_t speech) {
    agos_text_msg(which, colour, string);
    agos_speech(speech);
}

void script_fade_to_black() {
    banked_call(AGOS_EXTRA_BANK, fade_to_black_banked);
}

void script_effect(uint16_t id) {
    talk::effect = id;
    banked_call(AGOS_SOUND_BANK, effect_banked);
}

void script_sound_set(uint16_t set) {
    talk::sound_set = set;
    banked_call(AGOS_SOUND_BANK, sound_set_banked);
}

void vga_stop_sounds() {
    banked_call(AGOS_SOUND_BANK, sounds_stop_banked);
}

void vga_effect(uint16_t id) {
    script_effect(id);
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
    banked_call(AGOS_SOUND_BANK, speech_map_banked);
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

/// Bring a zone's figures in: its pixels staged in PACKED, then decoded whole.
/// In the bank with the decoders, because that is where the decoding is.
///
/// The store stages them, because the pixels share a file with the scripts and
/// asking for the scripts already brought them: no second scan of the card.
extern "C" CODE_BANK(AGOS_STORE_BANK) void load_zone_banked() {
    // The arena holds them, or remembers that the card has not got them. The
    // figure cache reserves nothing here: it is handed the pixels when a figure
    // is actually wanted, and keeps no pointer of its own.
    (void)agos::zone_pixels_of(zone_wanted);
    report::counts[report::ZONES_DECODED] =
        static_cast<uint16_t>(report::counts[report::ZONES_DECODED] + 1);
    report::counts[report::ZONES_HELD] = agos::zone_pixels.held();
    report::counts[report::ZONES_EMPTIED] = agos::zone_pixels.emptied();
}

/// The room on the screen, once its script has painted it.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void show_room_banked() {
    display_show();
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
    if (!local_text.begin())
        fault(FAULT_NO_FONT);

    banked_call(AGOS_STORE_BANK, side_files_banked);

    if (agos::read_game_file(text::WIN_FILE, atticmap::WINFONT, text::WIN_BYTES) !=
            text::WIN_BYTES ||
        agos::read_game_file(text::SAY_FILE, atticmap::SAYFONT, text::SAY_FONT_BYTES) !=
            text::SAY_FONT_BYTES)
        fault(FAULT_NO_FONT);

    // The picture and the panel start empty rather than holding whatever chip
    // RAM did. Nothing paints a backdrop until the script asks for one now, and
    // a machine that has been running something else shows that something in
    // every glyph the display fetches.
    // One job: the backdrop is 43,520 bytes, inside a DMA length. The split
    // this replaced was written for a larger region and had become an overrun
    // -- 65,535 bytes from the first job, and a second whose length underflowed
    // to 43,521 more, between them covering the panel and most of the pool.
    static_assert(chipmap::BACKDROP_BYTES <= 0xFFFF, "the backdrop wants a loop");
    agos::far_fill(chipmap::BACKDROP, 0, static_cast<uint16_t>(chipmap::BACKDROP_BYTES));
    agos::far_fill(chipmap::PANEL, 0, chipmap::PANEL_BYTES);
    agos::far_fill(atticmap::PANEL_MASTER, 0, atticmap::PANEL_MASTER_BYTES);
    // The line trace disarmed: Attic keeps the last run's arm byte.
    agos::far_fill(atticmap::LINE_TRACE, 0, atticmap::LINE_TRACE_HEADER);

    // And so do the boxes. Chip RAM comes up holding whatever it held, and a
    // box store that reads it as boxes has no room for the real ones: on the
    // machine this showed as fourteen spilled and none defined, where the host
    // had wiped its memory and seen nothing wrong.
    boxes.clear();
    figures.begin();
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
    banked_call(AGOS_COMPOSITE_BANK, composite_check_banked);
    // Which tune plays, and when, is the script's (127 PLAY_TUNE); the driver
    // is not ticked until it has a module, and the channels were silenced
    // before any of this began.
    DMA.auden = DMA_AUDEN;

    IRQ_VECTOR = reinterpret_cast<uint16_t>(&irq_entry);
    VICII.irr = 0x0f; // drop anything pending before unmasking
    VICII.imr = VIC_IRQ_RASTER;
    asm volatile("cli" ::: "p");

    banked_call(AGOS_STORE_BANK, store_open_banked);

    // No room is painted here any more. The script controls the display with
    // 96 PICTURE: loads a zone, runs its image script and shows it. The script's
    // zone animations set their own palettes, and the code does not.
    //
    // The display is turned on with nothing in it: a glyph of nought paints
    // nothing, so until the script asks for a picture the screen is the border.
    banked_call(AGOS_ROOM_BANK, show_room_banked);

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
            // release's animation scripts ask that way, across seventeen zones;
            // nothing ran any of them before.
            if (cutscene_ended != 0) {
                cutscene_ended = 0;
                banked_call(AGOS_SOUND_BANK, voice_stop_banked);
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
                    report::counts[report::TIMEOUTS_RUN] =
                        static_cast<uint16_t>(report::counts[report::TIMEOUTS_RUN] + ran);
                report::counts[report::TIMEOUTS_HELD] = vmstate::script.timeouts();
            }

            count_since(report::SCRIPT_LINES, script_began);
            report::counts[report::TABLES] = agos::store.tables_loaded();
            report::counts[report::ZONES] = agos::store.zones_loaded();
            report::counts[report::ZONES_FORCED] = agos::store.zones_forced();
        }
        if (const uint8_t key = held_key; key != 0) {
            held_key = 0;
            // Through the door, which sorts the rest out: the fixed region has no
            // room for a compare apiece.
            slot_key = key;
            banked_call(AGOS_SCRIPT_BANK, slot_key_banked);
        }
    }
}
