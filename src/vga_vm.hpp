// SPDX-License-Identifier: GPL-3.0-or-later

// The animation interpreter: the sprite list in draw order, the timer list
// that drives it, and the three tables scripts park themselves on.

#pragma once

#include "atticmap.hpp"
#include "script_vm.hpp"
#include "vga_zone.hpp"

#include <stdint.h>

#ifdef __mos__
#include <mapper.h>
#endif

namespace agos {

/// Sized to the game, not to the PC. ScummVM allocates 200/205/60 (agos.h:583,
/// :614, :584). Measured peaks through intro and first rooms: 43 sprites, 44
/// timers, 3 sleepers. Roughly twice the worst seen; a later scene may be
/// busier. Each list faults loudly when full (:414, :282, :317), so an
/// undersized bound announces itself rather than corrupting what follows.
inline constexpr uint8_t MAX_SPRITES = 96;
inline constexpr uint8_t MAX_TIMERS = 96;
inline constexpr uint8_t MAX_SLEEPERS = 24;

/// One tick of the 50 ms clock decrements every timer by this (event.cpp:233).
inline constexpr int16_t VGA_BASE_DELAY = 1;

/// Pathfinder routes, each a pointer into the script that declared it
/// (vc17_setPathfinderItem). ScummVM allocates 100; the release never names a
/// slot above 14, so anything past this is a fault rather than a bigger array.
inline constexpr uint8_t PATH_SLOTS = 16;

/// The (x,y) list a route is built from ends with this (vga_s1.cpp:56).
inline constexpr uint16_t PATH_END = 999;

/// Where vc48_setPathFinder reads its route, step and count, and where it
/// starts writing the halves it computes (vga.cpp).
enum : uint8_t {
    PATH_ROUTE_VAR = 12,
    PATH_INDEX_VAR = 13,
    PATH_COUNT_VAR = 14,
    PATH_FIRST_RESULT = 20
};

/// The redraw heartbeat re-arms itself to _frameCount and is never deleted;
/// vc27_resetSprite steps over it explicitly (vga.cpp:1124).
enum class VgaEvent : uint8_t { ANIMATE_INT, ANIMATE_EVENT };

/// A sprite, in draw order. Array order *is* priority order: vc23_setPriority
/// moves the entry to keep the array sorted ascending, and the draw walks it
/// from the front, so a higher priority is drawn later and therefore on top.
struct VgaSprite {
    uint16_t id = 0; // zero ends the list
    uint16_t image = 0;
    uint16_t priority = 0;
    uint16_t window = 0; // bit 15 marks it changed this tick
    int16_t x = 0;
    int16_t y = 0;
    uint8_t palette = 0;
    uint8_t flags = 0;
    uint8_t zone = 0;
};

struct VgaTimer {
    int16_t delay = 0; // zero ends the list
    Place code = NOWHERE;
    uint16_t id = 0;
    uint8_t zone = 0;
    VgaEvent type = VgaEvent::ANIMATE_EVENT;
};

/// A script parked until something releases it: waiting on a sync id, on a
/// sprite ending, or on one stopping.
struct VgaSleeper {
    uint16_t ident = 0; // zero ends the list
    Place code = NOWHERE;
    uint16_t id = 0;
    uint8_t zone = 0;
};

/// A DRAW's flags, as the script carries them (ScummVM's DrawFlags,
/// vga.h:100-106). Only the one this renderer acts on is named: set, every
/// pixel is painted, and measured over the release it is set on 177 DRAWs and
/// on none whose palette block is above nought -- so it marks the room's own
/// backdrop rather than anything about the image's size.
inline constexpr uint16_t DRAW_OPAQUE = 0x02; // kDFNonTrans

/// kDFFlip: the figure is drawn mirrored, which is how an actor faces the
/// other way. The engine mirrors the source bitmap before drawing it
/// (vc10_flip, vga.cpp:710); the RRB mirrors each glyph instead and the row
/// lays the columns in the other order, which needs no second copy of the art.
inline constexpr uint16_t DRAW_FLIP = 0x01; // kDFFlip

/// kDFMasked (vga.h:105), as MASK sets it: the sprite draws nothing of its
/// own, and where its image is set the room's clean picture covers the
/// sprites before it. MASK's kDFSkipStoreBG beside it is left out: it is
/// about restoring a background, which nothing here saves.
inline constexpr uint16_t DRAW_MASKED = 0x20;

/// The text window, the room's, and the first of the room's sub-windows: the
/// three that draw into the room's buffer (gfx.cpp:688).
inline constexpr uint8_t TEXT_WINDOW = 3, ROOM_WINDOW = 4, FIRST_ROOM_SUB_WINDOW = 10;

/// Marks a sprite as changed since the last draw (vga.cpp:1006).
inline constexpr uint16_t WINDOW_CHANGED = 0x8000;

/// An opcode behind a tier that does not exist: drawing, palettes, windows,
/// sound, the mouse.
void vga_unimplemented(uint8_t opcode);

#ifdef __mos__
/// The doors into the banks the animation VM lives in; which bank is which is
/// in banks.hpp. It will not fit the fixed region beside everything else, so
/// the dispatch goes in one and the tick that drives it in another, while the
/// reset shares the store's bank -- 288 bytes that never wanted one of its own
/// and that runs at the same moment the store loads.
///
/// banked_call takes a function of no arguments, so the opcode travels in a
/// member and the trampolines name the one animation VM the machine has.
extern "C" void vga_execute_banked();
extern "C" void vga_tick_banked();
extern "C" void vga_reset_banked();
#endif

/// One frame's display list, in the order it is drawn. This is what Tier 5
/// consumes; until then it is how a test sees what the VM decided.
void vga_draw(const VgaSprite* sprites, uint8_t count);

void note_lost(uint16_t id, uint8_t zone, uint32_t pc);

/// The zone holding this sprite's scripts, loaded by whoever owns the files.
VgaZone* vga_zone(uint8_t number);

/// One image painted where the script says, with the palette block it says.
///
/// This is DRAW, which is what actually puts a picture on the screen; the
/// sprite list above says what is alive, not what is drawn. A four-plane
/// image's colour index reaches the display palette OR'd with `block * 16`
/// (vga.cpp:630, gfx.cpp:793), so the block travels with the request.
void vga_paint(uint8_t zone, uint16_t image, uint8_t block, int16_t x, int16_t y, uint16_t flags);

/// Whether the line's voice is still playing.
[[nodiscard]] bool vga_voice_playing();
/// STOP_ALL_SOUNDS: speech and effect both (vc29_stopAllSounds,
/// vga.cpp:1172-1176).
void vga_stop_sounds();
/// PLAY_SOUND: an effect from the sound set, as PLAY_EFFECT plays one.
void vga_effect(uint16_t id);
/// SET_MOUSE_ON and SET_MOUSE_OFF: the pointer shown, or hidden with any
/// press not yet acted on dropped (vc33, vc34, vga.cpp:1215-1233).
void vga_mouse(bool shown);
/// ENABLE_BOX: a disabled box answers clicks again (vc51, vga_e2.cpp:166).
void vga_enable_box(uint16_t id);
/// MOVE_BOX: a box shifted by a step, not set to a place, unlike the
/// script's moveBox (vc55, vga_e2.cpp:333-349).
void vga_move_box(uint16_t id, int16_t dx, int16_t dy);

/// The window's picture becomes another image: the ground it had is cleared.
/// Running the image script is the VM's own work, next door in execute().
void vga_window_image(uint16_t window);

/// Load a palette: sixteen entries at display `block * 16` from the zone's
/// source block, or thirty-two when the block is nought (vga_s1.cpp:96).
void vga_palette(uint8_t zone, uint8_t block, uint8_t source);

/// The display to black and back: the only fades Simon 1 installs
/// (vga_s1.cpp:43-44). Opcodes 1 and 4 are dummies in the engine too
/// (vga.cpp:392, :445).
void vga_fade_out(uint8_t window);
void vga_fade_in();

class VgaVm {
  public:
    void reset(ScriptVm& script) {
        script_ = &script;
#ifdef __mos__
        banked_call(AGOS_STORE_BANK, vga_reset_banked);
#else
        clear();
#endif
    }

    /// Everything the VM holds, emptied. Three hundred entries of it, which is
    /// why it is behind a door of its own rather than in the fixed region.
    void clear() {
        for (uint8_t i = 0; i < MAX_SPRITES; ++i)
            sprites_[i] = VgaSprite();
        for (uint8_t i = 0; i < MAX_TIMERS; ++i)
            timers_[i] = VgaTimer();
        for (uint8_t i = 0; i < MAX_SLEEPERS; ++i)
            wait_sync_[i] = wait_end_[i] = on_stop_[i] = VgaSleeper();
        for (uint8_t i = 0; i < PATH_SLOTS; ++i)
            path_[i] = NOWHERE;
        // initialVideoWindows_Simon (agos.cpp:718): only window 2 is off the
        // origin. Set once with the rest, as the engine sets them once.
        far_fill(atticmap::WINDOW_ORIGINS, 0, atticmap::WINDOW_ORIGINS_BYTES);
        far_write(atticmap::WINDOW_ORIGINS, INITIAL_WINDOWS, sizeof INITIAL_WINDOWS);
        window_ = 0;
        frame_count_ = 1;
        ticks_ = 0;
        halted_ = false;
        unimplemented_ = 0;
        // The redraw heartbeat, the one entry that is never deleted.
        add_event(static_cast<int16_t>(frame_count_), VgaEvent::ANIMATE_INT, NOWHERE, 0, 0);
    }

    /// animate(): put a sprite in the list and start its script on the *next*
    /// tick, not here (gfx.cpp:1112). A sprite already loaded is left alone.
    void animate(
        uint16_t window, uint8_t zone, uint16_t sprite_id, int16_t x, int16_t y, uint8_t palette);

    /// processVgaEvents: one tick of the twenty a second Simon 1 asks for --
    /// _vgaPeriod is 50 *milliseconds* (AGOSEngine_Simon1::setupGame,
    /// agos.cpp:818), which is a period and not a rate.
    void tick() {
#ifdef __mos__
        banked_call(AGOS_VGA_TICK_BANK, vga_tick_banked);
#else
        tick_now();
#endif
    }

    /// HALT_ANIMATION and RESTART_ANIMATION (88, 89): no tick runs between
    /// them, the redraw included, and a halt puts every pending script ten
    /// further off each time (o_haltAnimation, script.cpp:501-516; the lock
    /// stops processVgaEvents, event.cpp:600).
    void halt(bool on) {
        constexpr int16_t HALT_DELAY = 10;
        if (on)
            for (uint8_t at = 0; at < MAX_TIMERS && timers_[at].delay; ++at)
                if (timers_[at].type == VgaEvent::ANIMATE_EVENT)
                    timers_[at].delay = static_cast<int16_t>(timers_[at].delay + HALT_DELAY);
        halted_ = on;
    }

    /// Release whatever is waiting on this id, which is what a script's SYNC
    /// does (120) and what the animation VM's own opcode 15 does.
    void send_sync(uint16_t ident) {
        release(wait_sync_, ident);
        if (script_ == nullptr)
            return;
        script_->synced(ident);
    }

    /// Every sprite gone, every rendezvous with it, which is what a script's
    /// KILL_ANIMATE asks for (100 -> vc27_resetSprite, vga.cpp:1124). The
    /// redraw heartbeat is the one timer that stays.
    void reset_sprites() {
        if (script_ != nullptr)
            script_->forget_syncs();
        for (uint8_t at = 0; at < MAX_SPRITES; ++at)
            sprites_[at] = VgaSprite();
        for (uint8_t at = 0; at < MAX_SLEEPERS; ++at)
            wait_sync_[at] = wait_end_[at] = on_stop_[at] = VgaSleeper();
        for (uint8_t at = 0; at < MAX_TIMERS && timers_[at].delay;)
            if (timers_[at].type == VgaEvent::ANIMATE_INT)
                ++at;
            else
                delete_event(at);
    }

    [[nodiscard]] uint8_t sprite_count() const;
    [[nodiscard]] const VgaSprite* sprites() const {
        return sprites_;
    }

    /// Run an image script by its id, the way vc2_call reaches one.
    ///
    /// The game's own scripts start these; this is how a caller with no game
    /// running can ask a room to paint itself, which is what its image script
    /// does -- the palettes it wants and the images it draws, in order.
    /// Safe from inside a running script as well as from outside one.
    ///
    /// run_script overwrites pc_, the current sprite and zone, the file the
    /// opcodes read and the yield -- it restores none of them, because the
    /// script it runs is normally the only one there is. So the caller's are
    /// put back here, the way CALL does it (opcode 2), and the yield with
    /// them: a nested script that parks itself must not park the script that
    /// ran it. Without this, an opcode that ran an image script would resume
    /// the outer one at the inner one's program counter.
    void run_image(uint8_t zone, uint16_t image_id) {
        VgaZone* file = vga_zone(zone);
        if (file == nullptr)
            return;
        const Place code = file->image_script(image_id);
        if (code == NOWHERE)
            return;
        const Place resume = pc_;
        const uint16_t id = current_id_;
        const uint8_t outer_zone = current_zone_;
        const uint8_t outer_file = file_zone_;
        run_script(code, image_id, zone, zone);
        pc_ = resume;
        current_id_ = id;
        current_zone_ = outer_zone;
        file_zone_ = outer_file;
        yielded_ = false;
    }
    /// The route point nearest @p x, @p y, as the slot (from 1) in the high
    /// byte and the point in the low; nought when no route is declared.
    /// os1_getPathPosn's measure (script_s1.cpp:433): the point stands 12
    /// above its y, the nearer axis counts a quarter and the farther in full,
    /// and a tie goes to @p preferred, the route walked now.
    [[nodiscard]] uint16_t nearest_route_point(uint16_t x, uint16_t y, uint16_t preferred) const {
        constexpr int16_t STANDS_ABOVE = 12;
        uint16_t best = 0, best_distance = UINT16_MAX;
        for (uint8_t slot = 0; slot < PATH_SLOTS; ++slot) {
            Place at = path_[slot];
            if (at == NOWHERE)
                continue;
            for (uint8_t point = 0;; ++point, at += 4) {
                const uint16_t point_x = far_read16(at);
                if (point_x == PATH_END)
                    break;
                const int16_t dx = static_cast<int16_t>(point_x - x);
                const int16_t dy = static_cast<int16_t>(far_read16(at + 2) - STANDS_ABOVE - y);
                uint16_t across = static_cast<uint16_t>(dx < 0 ? -dx : dx);
                uint16_t down = static_cast<uint16_t>(dy < 0 ? -dy : dy);
                if (across < down) {
                    across = static_cast<uint16_t>(across / 4);
                    down = static_cast<uint16_t>(down * 4);
                }
                const uint16_t distance = static_cast<uint16_t>(across + down / 4);
                if (distance < best_distance ||
                    (distance == best_distance && slot + 1u == preferred)) {
                    best_distance = distance;
                    best = static_cast<uint16_t>((slot + 1u) << 8 | point);
                }
            }
        }
        return best;
    }

    /// stopAnimate: a sprite and everything waiting on it, gone.
    void stop(uint16_t sprite_id) {
        stop_animation(static_cast<uint8_t>(sprite_id / SPRITES_PER_ZONE), sprite_id);
    }

    /// The window the next DRAWs land in, as PICTURE and SET_WINDOW_IMAGE set
    /// it before their script runs (setWindowImage, gfx.cpp:1384).
    void use_window(uint16_t window) {
        if (window >= atticmap::VIDEO_WINDOWS) {
            vga_fault(Fault::VGA_WINDOW_RANGE, window);
            window = 0;
        }
        window_ = static_cast<uint8_t>(window);
    }

    /// Whether any script of this zone is still live: drawn, timed, or parked
    /// waiting on something. A parked script keeps a pointer into the file it
    /// was parked in, so the store may not load another zone over it.
    [[nodiscard]] bool zone_running(uint8_t zone) const;

    [[nodiscard]] uint8_t timer_count() const;

    [[nodiscard]] uint16_t ticks() const {
        return ticks_;
    }
    [[nodiscard]] uint16_t unimplemented() const {
        return unimplemented_;
    }
    [[nodiscard]] uint16_t frame_count() const {
        return frame_count_;
    }

    /// What the trampolines came in to do. Public because the doors into the
    /// banks are free functions, and there is nothing else they could mean.
    void execute_pending() {
        execute(pending_);
    }
    void tick_now();

  private:
    // ------------------------------------------------------------- operands

    uint16_t read_word() {
        const uint16_t value = far_read16(pc_);
        pc_ += 2;
        return value;
    }
    /// vcReadVarOrWord: a negative word names a variable by its own negation.
    uint16_t read_var_or_word() {
        const int16_t value = static_cast<int16_t>(read_word());
        return value < 0 ? read_var(static_cast<uint16_t>(-value)) : static_cast<uint16_t>(value);
    }
    [[nodiscard]] uint16_t read_var(uint16_t number) const {
        return static_cast<uint16_t>(script_->variable(static_cast<uint8_t>(number)));
    }
    void write_var(uint16_t number, uint16_t value) {
        script_->set_variable(static_cast<uint8_t>(number), static_cast<int16_t>(value));
    }

    /// vcSkipNextInstruction: by the length table, opcode 17's known wrong entry
    /// included, because that is what the engine steps by (vga.cpp:293).
    void skip_next_instruction() {
        const uint16_t opcode = far_read16(pc_);
        pc_ += 2;
        if (opcode < VIDEO_OPCODES)
            pc_ += VIDEO_PARAM_LEN[opcode];
        else
            vga_fault(Fault::VGA_SKIPPED_OPCODE, opcode);
    }

    // -------------------------------------------------------------- the VM

    /// Run a script to its next yield. @p file_zone is the zone whose file the
    /// opcodes read palettes and images from, which is the sprite's own zone
    /// except inside a CALL.
    void run_script(Place code, uint16_t sprite_id, uint8_t zone, uint8_t file_zone);
    void execute(uint8_t opcode);

    [[nodiscard]] VgaSprite* current_sprite();
    void add_event(int16_t delay, VgaEvent type, Place code, uint16_t id, uint8_t zone);
    void delete_event(uint8_t at);
    void park(VgaSleeper* table, uint16_t ident);
    static void compact(VgaSleeper* table, uint8_t at);
    void release(VgaSleeper* table, uint16_t ident);
    void halt_sprite();
    void stop_animation(uint8_t zone, uint16_t sprite_id);
    void draw_sprites();

    ScriptVm* script_ = nullptr;

    VgaSprite sprites_[MAX_SPRITES];
    VgaTimer timers_[MAX_TIMERS];
    VgaSleeper wait_sync_[MAX_SLEEPERS];
    VgaSleeper wait_end_[MAX_SLEEPERS];
    VgaSleeper on_stop_[MAX_SLEEPERS];

    Place path_[PATH_SLOTS] = {};
    Place pc_ = NOWHERE;
    bool yielded_ = false;
    uint16_t current_id_ = 0;
    uint8_t current_zone_ = 0;
    /// The zone whose file the opcodes read: the sprite's own, until a CALL
    /// goes into another zone's script.
    uint8_t file_zone_ = 0;

    /// Where window @p w is kept: x, y, then height.
    [[nodiscard]] static Place window_origin(uint8_t w) {
        return atticmap::WINDOW_ORIGINS + static_cast<uint8_t>(w * atticmap::WINDOW_ENTRY_BYTES);
    }
    /// initialVideoWindows_Simon (agos.cpp:718), the five the engine starts
    /// with; the rest are nought until SET_SUB_WINDOW. Window 4, the room, is
    /// 134 lines, which is what keeps its DRAWs out of the panel.
    static constexpr uint8_t INITIAL_WINDOWS[] = {
        0, 0, 200, 0, 0, 136, 17, 0, 136, 0, 0, 200, 0, 0, 134};
    /// _windowNum: which window a DRAW is placed in.
    uint8_t window_ = 0;

    uint8_t pending_ = 0;
    uint16_t frame_count_ = 1;
    uint16_t ticks_ = 0;
    bool halted_ = false;
    uint16_t unimplemented_ = 0;
    uint8_t next_timer_ = 0; // where processVgaEvents resumes after a delete
};

// ---------------------------------------------------------------- definitions

inline bool VgaVm::zone_running(uint8_t zone) const {
    for (uint8_t at = 0; at < MAX_SPRITES && sprites_[at].id; ++at)
        if (sprites_[at].zone == zone)
            return true;
    for (uint8_t at = 0; at < MAX_TIMERS && timers_[at].delay; ++at)
        if (timers_[at].zone == zone)
            return true;
    const VgaSleeper* const tables[] = {wait_sync_, wait_end_, on_stop_};
    for (const VgaSleeper* table : tables)
        for (uint8_t at = 0; at < MAX_SLEEPERS && table[at].ident; ++at)
            if (table[at].zone == zone)
                return true;
    return false;
}

inline uint8_t VgaVm::sprite_count() const {
    uint8_t count = 0;
    while (count < MAX_SPRITES && sprites_[count].id)
        ++count;
    return count;
}

inline uint8_t VgaVm::timer_count() const {
    uint8_t count = 0;
    while (count < MAX_TIMERS && timers_[count].delay)
        ++count;
    return count;
}

inline void VgaVm::add_event(int16_t delay, VgaEvent type, Place code, uint16_t id, uint8_t zone) {
    uint8_t at = 0;
    while (at < MAX_TIMERS && timers_[at].delay)
        ++at;
    if (at + 1 >= MAX_TIMERS) {
        vga_fault(Fault::VGA_TIMERS_FULL, at);
        return;
    }
    timers_[at] = VgaTimer{delay, code, id, zone, type};
    timers_[at + 1] = VgaTimer();
}

inline void VgaVm::delete_event(uint8_t at) {
    // The walk in tick() holds an index, so it has to move with the shift.
    if (at + 1 <= next_timer_)
        --next_timer_;

    // At least one shift, then on while what was just copied is a live entry.
    // Testing first instead reads the entry being deleted, whose delay has just
    // reached nought -- so nothing moved, a zero was left in the middle of the
    // list, and every timer past it became unreachable. deleteVgaEvent is a
    // do-while for this reason (vga.cpp:174).
    do {
        timers_[at] = timers_[at + 1];
        ++at;
    } while (at + 1 < MAX_TIMERS && timers_[at].delay);
}

inline VgaSprite* VgaVm::current_sprite() {
    for (uint8_t at = 0; at < MAX_SPRITES && sprites_[at].id; ++at)
        if (sprites_[at].id == current_id_ && sprites_[at].zone == current_zone_)
            return &sprites_[at];
    return nullptr;
}

inline void VgaVm::park(VgaSleeper* table, uint16_t ident) {
    uint8_t at = 0;
    while (at < MAX_SLEEPERS && table[at].ident)
        ++at;
    if (at + 1 >= MAX_SLEEPERS) {
        vga_fault(Fault::VGA_SLEEPERS_FULL, at);
        return;
    }
    table[at] = VgaSleeper{ident, pc_, current_id_, current_zone_};
    table[at + 1] = VgaSleeper();
}

/// Close the hole one entry leaves, keeping the table null-terminated.
inline void VgaVm::compact(VgaSleeper* table, uint8_t at) {
    for (uint8_t i = at; i + 1 < MAX_SLEEPERS && table[i].ident; ++i)
        table[i] = table[i + 1];
}

/// Everything parked on this id starts again on the next tick, and leaves the
/// table (vc15_sync, vga.cpp:790).
inline void VgaVm::release(VgaSleeper* table, uint16_t ident) {
    for (uint8_t at = 0; at < MAX_SLEEPERS && table[at].ident;) {
        if (table[at].ident != ident) {
            ++at;
            continue;
        }
        add_event(
            VGA_BASE_DELAY, VgaEvent::ANIMATE_EVENT, table[at].code, table[at].id, table[at].zone);
        compact(table, at);
    }
}

inline void VgaVm::halt_sprite() {
    release(wait_end_, current_id_);

    // On-stop is the one that does not simply resume: it starts another sprite
    // where this one stood (checkOnStopTable, vga.cpp:838).
    for (uint8_t at = 0; at < MAX_SLEEPERS && on_stop_[at].ident;) {
        if (on_stop_[at].ident != current_id_) {
            ++at;
            continue;
        }
        if (const VgaSprite* here = current_sprite())
            animate(here->window, here->zone, on_stop_[at].id, here->x, here->y, here->palette);
        compact(on_stop_, at);
    }

    if (VgaSprite* sprite = current_sprite()) {
        const uint8_t at = static_cast<uint8_t>(sprite - sprites_);
        for (uint8_t i = at; i + 1 < MAX_SPRITES && sprites_[i].id; ++i)
            sprites_[i] = sprites_[i + 1];
    }
    yielded_ = true;
}

// Inlined into each bank that stops a sprite: shared, it lands in the fixed
// region, which has no room for it.
[[gnu::always_inline]] inline void VgaVm::stop_animation(uint8_t zone, uint16_t sprite_id) {
    const uint16_t old_id = current_id_;
    const uint8_t old_zone = current_zone_;
    const Place old_pc = pc_;
    current_id_ = sprite_id;
    current_zone_ = zone;

    for (uint8_t at = 0; at < MAX_SLEEPERS && wait_sync_[at].ident; ++at)
        if (wait_sync_[at].id == sprite_id && wait_sync_[at].zone == zone) {
            compact(wait_sync_, at);
            break;
        }

    if (current_sprite()) {
        halt_sprite();
        for (uint8_t at = 0; at < MAX_TIMERS && timers_[at].delay; ++at)
            if (timers_[at].id == sprite_id && timers_[at].zone == zone) {
                delete_event(at);
                break;
            }
    }

    current_id_ = old_id;
    current_zone_ = old_zone;
    pc_ = old_pc;
    yielded_ = false;
}

inline void VgaVm::draw_sprites() {
    const uint8_t count = sprite_count();
    for (uint8_t at = 0; at < count; ++at)
        sprites_[at].window = static_cast<uint16_t>(sprites_[at].window & ~WINDOW_CHANGED);
    // Drawing leaves the last sprite's window in force (animateSprites,
    // draw.cpp:103), so a SET_WINDOW 5 does not outlive the frame and send the
    // room's later DRAWs into the panel.
    if (count != 0)
        use_window(sprites_[count - 1].window);
    vga_draw(sprites_, count);
}

inline void VgaVm::animate(
    uint16_t window, uint8_t zone, uint16_t sprite_id, int16_t x, int16_t y, uint8_t palette) {
    for (uint8_t at = 0; at < MAX_SPRITES && sprites_[at].id; ++at)
        if (sprites_[at].id == sprite_id && sprites_[at].zone == zone)
            return; // isSpriteLoaded: already there

    uint8_t at = 0;
    while (at < MAX_SPRITES && sprites_[at].id)
        ++at;
    if (at + 1 >= MAX_SPRITES) {
        vga_fault(Fault::VGA_SPRITES_FULL, at);
        return;
    }
    // The entry before the script, as the engine does (gfx.cpp:1123-1139): it
    // fills the slot and only then goes looking for the zone.
    sprites_[at] = VgaSprite{sprite_id, 0, 0, window, x, y, palette, 0, zone};
    sprites_[at + 1] = VgaSprite();

    // Where the engine loads a zone that is not resident and tries again
    // (gfx.cpp:1141-1155), this only asks. A sprite whose zone never arrives is
    // left in the list with no script -- drawn, never run -- and that is the
    // shape of what stalls the intro.
    VgaZone* file = vga_zone(zone);
    const Place code = file ? file->animation_script(sprite_id) : NOWHERE;
    if (code == NOWHERE) {
        vga_fault(Fault::VGA_NO_ANIMATION, sprite_id);
        return;
    }
    // Next tick, not now: a sprite's script never runs inside the call that
    // created it (gfx.cpp:1112).
    add_event(VGA_BASE_DELAY, VgaEvent::ANIMATE_EVENT, code, sprite_id, zone);
}

inline void VgaVm::tick_now() {
    if (halted_)
        return;
    ++ticks_;
    for (uint8_t at = 0; at < MAX_TIMERS && timers_[at].delay;) {
        timers_[at].delay = static_cast<int16_t>(timers_[at].delay - VGA_BASE_DELAY);
        if (timers_[at].delay > 0) {
            ++at;
            continue;
        }
        if (timers_[at].type == VgaEvent::ANIMATE_INT) {
            timers_[at].delay = static_cast<int16_t>(frame_count_);
            draw_sprites();
            ++at;
            continue;
        }
        const VgaTimer entry = timers_[at];
        next_timer_ = static_cast<uint8_t>(at + 1);
        delete_event(at);
        // A timer with no script runs from address zero, reading chip RAM as
        // opcodes and raising a fault. Refusing would crash; the fault instead
        // warns of state corruption elsewhere. The real bug is a timer blanked
        // by an earlier operation, not found yet.
        run_script(entry.code, entry.id, entry.zone, entry.zone);
        at = next_timer_;
    }
}

inline void VgaVm::run_script(Place code, uint16_t sprite_id, uint8_t zone, uint8_t file_zone) {
    pc_ = code;
    current_id_ = sprite_id;
    current_zone_ = zone;
    file_zone_ = file_zone;
    yielded_ = false;
    while (!yielded_) {
        const uint16_t opcode = far_read16(pc_);
        pc_ += 2;
        if (opcode == 0)
            return; // the terminator, handled before dispatch
        if (opcode >= VIDEO_OPCODES || !((VIDEO_INSTALLED >> opcode) & 1)) {
            note_lost(current_id_, current_zone_, pc_ - 2);
            vga_fault(Fault::VGA_OPCODE_MISSING, opcode);
            return;
        }
#ifdef __mos__
        // Through the window: everything the dispatch needs is either in the bank
        // with it or in the fixed region, which is always mapped.
        pending_ = static_cast<uint8_t>(opcode);
        // CALL and SET_WINDOW_IMAGE run a script from inside this one, so this
        // door is re-entered through the bank it calls: see banked_reenter.
        banked_reenter<vga_execute_banked>(AGOS_VGA_BANK);
#else
        execute(static_cast<uint8_t>(opcode));
#endif
    }
}

/// The opcodes that move the VM's own state: the sprite list, the timers, the
/// rendezvous tables, the variables and bits it shares with the script VM, and
/// the control flow. Drawing, palettes, windows and sound leave through the
/// hook, with their operands consumed.
inline void VgaVm::execute(uint8_t opcode) {
    switch (opcode) {
        case 10: { // DRAW
            // Ten bytes: the image, then the palette block as the *low* byte of a
            // word (vga.cpp:602 takes _vcPtr[1] and steps two), then x, y and flags.
            const int16_t named = static_cast<int16_t>(read_word());
            const uint8_t block = static_cast<uint8_t>(read_word());
            const int16_t x = static_cast<int16_t>(read_word());
            const int16_t y = static_cast<int16_t>(read_word());
            const uint16_t flags = read_word();

            // A negative image names a variable holding the real one, and image zero
            // draws nothing at all (vga.cpp:619).
            const uint16_t image =
                named < 0 ? read_var(static_cast<uint16_t>(-named)) : static_cast<uint16_t>(named);
            // Placed in the window in force: xoffs = (vlut[0] * 2 + x) * 8,
            // yoffs = vlut[1] + y (gfx.cpp:940). The paint decides from the y whether
            // that is the picture or the panel below it.
            //
            // Windows 3, 4 and 10 on draw into the room's own buffer, so they are
            // placed from the room window's origin, not the screen's
            // (gfx.cpp:688-705); the picture is that buffer. The intro's bedroom
            // moves window 4 to line 108 (zone 4, sprite script 413) and its DRAWs
            // and sprites stay where the room's are.
            // A piece starting at or below the window's foot is clipped whole
            // (drawImage_clip, gfx.cpp:208-216).
            const Place window = window_origin(window_);
            if (image != 0 && y < int16_t{far_read8(window + 2)}) {
                // x is the high byte, as stored (far_read16 is big-endian).
                const uint16_t origin = far_read16(window);
                int16_t across = static_cast<int16_t>(origin >> 8);
                int16_t down = static_cast<int16_t>(origin & 0xFF);
                if (window_ == TEXT_WINDOW || window_ == ROOM_WINDOW ||
                    window_ >= FIRST_ROOM_SUB_WINDOW) {
                    const uint16_t room = far_read16(window_origin(ROOM_WINDOW));
                    across = static_cast<int16_t>(across - static_cast<int16_t>(room >> 8));
                    down = static_cast<int16_t>(down - static_cast<int16_t>(room & 0xFF));
                }
                vga_paint(file_zone_,
                    image,
                    block,
                    static_cast<int16_t>(x + across * 2),
                    static_cast<int16_t>(y + down),
                    flags);
            }
            break;
        }

        case 26: { // SET_SUB_WINDOW: a window's place and height (vga.cpp:1073)
            const uint16_t window = read_word();
            const uint8_t x = static_cast<uint8_t>(read_word());
            const uint8_t y = static_cast<uint8_t>(read_word());
            pc_ += 2; // the width only clips across
            const uint8_t height = static_cast<uint8_t>(read_word());
            if (window < atticmap::VIDEO_WINDOWS) {
                const Place origin = window_origin(static_cast<uint8_t>(window));
                far_write8(origin, x);
                far_write8(origin + 1, y);
                far_write8(origin + 2, height);
            } else
                vga_fault(Fault::VGA_WINDOW_RANGE, window);
            break;
        }
        case 31: // SET_WINDOW (vga.cpp:1183)
            use_window(read_word());
            break;

        case 62: // FASTFADEOUT: speech and effect stop first, fade or not
            // (vc62_fastFadeOut, vga_ww.cpp:198-199)
            vga_stop_sounds();
            vga_fade_out(window_);
            break;
        case 63: // FASTFADEIN
            vga_fade_in();
            break;

        case 22: { // SET_PALETTE
            const uint8_t block = static_cast<uint8_t>(read_word());
            const uint8_t source = static_cast<uint8_t>(read_word());
            vga_palette(file_zone_, block, source);
            break;
        }

        case 36: { // SET_WINDOW_IMAGE: the window's picture becomes this image
            // The image first, then the window (vc36_setWindowImage, vga.cpp:1044).
            // What the engine does with it is setImage (gfx.cpp:1381 -> :1218): find
            // the zone, load it if it is not resident, clear the window, and run that
            // image script. All three are things this VM already does -- CALL below
            // runs an image script the same way, and vga_zone fetches a zone's
            // scripts when they are not here.
            //
            // This is the whole of the intro's first scene. Zone 8's image script 800
            // is one instruction, SET_WINDOW_IMAGE 801 4, and script 801 is four
            // palettes and the sprites 802, 814, 821 and 700 that the scene is made
            // of. Skipped, none of them exists and nobody walks on.
            //
            // Not copied: the engine shortens its redraw timer to two ticks
            // (gfx.cpp:1388). This draws the sprite list every frame, so there is no
            // delay to shorten.
            const uint16_t image_id = read_word();
            const uint16_t window = read_word();
            vga_window_image(window);
            use_window(window);
            run_image(static_cast<uint8_t>(image_id / SPRITES_PER_ZONE), image_id);
            break;
        }

        case 2: { // CALL: an image script, run inline, not a call and return pair
            const uint16_t image_id = read_var_or_word();
            const uint8_t called = static_cast<uint8_t>(image_id / SPRITES_PER_ZONE);
            VgaZone* file = vga_zone(called);
            const Place code = file ? file->image_script(image_id) : NOWHERE;
            if (code == NOWHERE) {
                vga_fault(Fault::VGA_NO_IMAGE_SCRIPT, image_id);
                break;
            }
            const Place resume = pc_;
            const uint16_t id = current_id_;
            const uint8_t zone = current_zone_;
            // The file the called script reads its palettes and images from is the
            // called zone's, while the sprite it belongs to stays the caller's
            // (setImage saves and restores _curVgaFile1/2 around it, gfx.cpp:1316,
            // and leaves _vgaCurZoneNum alone). Reading them from the caller's zone
            // painted the first thing on screen, "Adventure Soft presents", in
            // another zone's colours.
            const uint8_t outer_file = file_zone_;
            run_script(code, id, zone, called);
            file_zone_ = outer_file;
            pc_ = resume;
            current_id_ = id;
            current_zone_ = zone;
            yielded_ = false;
            break;
        }
        case 3: { // NEW_SPRITE
            const uint16_t window = read_word();
            const uint16_t sprite_id = read_word();
            const int16_t x = static_cast<int16_t>(read_word());
            const int16_t y = static_cast<int16_t>(read_word());
            const uint16_t palette = read_word();
            const Place resume = pc_;
            const uint16_t id = current_id_;
            const uint8_t zone = current_zone_;
            animate(window,
                static_cast<uint8_t>(sprite_id / SPRITES_PER_ZONE),
                sprite_id,
                x,
                y,
                static_cast<uint8_t>(palette));
            pc_ = resume;
            current_id_ = id;
            current_zone_ = zone;
            break;
        }

        // --- conditions, each skipping the next instruction when it fails
        case 5: { // IF_EQUAL
            const uint16_t left = read_var(read_word());
            if (left != read_word())
                skip_next_instruction();
            break;
        }
        case 6:   // IF_OBJECT_HERE
        case 7: { // IF_OBJECT_NOT_HERE
            const uint16_t item = script_->vga_object(static_cast<uint8_t>(read_word()));
            const bool here =
                item == NO_ITEM || script_->db().item(item).parent() == script_->player_parent();
            if (here == (opcode == 7))
                skip_next_instruction();
            break;
        }
        case 8: { // IF_OBJECT_IS_AT
            const uint16_t a = script_->vga_object(static_cast<uint8_t>(read_word()));
            const uint16_t b = script_->vga_object(static_cast<uint8_t>(read_word()));
            const bool at = a == NO_ITEM || b == NO_ITEM || script_->db().item(a).parent() == b;
            if (!at)
                skip_next_instruction();
            break;
        }
        case 9: { // IF_OBJECT_STATE_IS
            const uint16_t item = script_->vga_object(static_cast<uint8_t>(read_word()));
            const int16_t state = static_cast<int16_t>(read_word());
            const bool is = item == NO_ITEM || script_->db().item(item).state() == state;
            if (!is)
                skip_next_instruction();
            break;
        }
        case 38: { // IF_VAR_NOT_ZERO
            if (read_var(read_word()) == 0)
                skip_next_instruction();
            break;
        }
        case 43:   // IF_BIT_SET
        case 44: { // IF_BIT_CLEAR
            if (script_->bit(read_word()) == (opcode == 44))
                skip_next_instruction();
            break;
        }
        case 59: // IF_SPEECH: whether the voice is still going (vc59)
            if (!vga_voice_playing())
                skip_next_instruction();
            break;

        // --- the sprite this script belongs to
        case 13:   // SET_SPRITE_OFFSET_X
        case 14: { // SET_SPRITE_OFFSET_Y
            const int16_t by = static_cast<int16_t>(read_word());
            if (VgaSprite* sprite = current_sprite()) {
                (opcode == 13 ? sprite->x : sprite->y) =
                    static_cast<int16_t>((opcode == 13 ? sprite->x : sprite->y) + by);
                sprite->window |= WINDOW_CHANGED;
            }
            break;
        }
        case 37: { // SET_SPRITE_OFFSET_Y, by variable
            const uint16_t by = read_var(read_word());
            if (VgaSprite* sprite = current_sprite()) {
                sprite->y = static_cast<int16_t>(sprite->y + static_cast<int16_t>(by));
                sprite->window |= WINDOW_CHANGED;
            }
            break;
        }
        case 45:   // SET_SPRITE_X
        case 46: { // SET_SPRITE_Y
            const int16_t to = static_cast<int16_t>(read_var(read_word()));
            if (VgaSprite* sprite = current_sprite()) {
                (opcode == 45 ? sprite->x : sprite->y) = to;
                sprite->window |= WINDOW_CHANGED;
            }
            break;
        }
        case 24:   // SET_SPRITE_XY: a new image and a step, in one instruction
        case 61: { // MASK: the same, without flags, and the sprite becomes a mask
                   // (vc61_setMaskImage, vga_s1.cpp:225-236)
            const uint16_t image = read_var_or_word();
            const int16_t dx = static_cast<int16_t>(read_word());
            const int16_t dy = static_cast<int16_t>(read_word());
            // A word on disk, but the release only ever writes 0, 1 or 4 -- measured
            // over all 36,830 occurrences -- so the sprite keeps a byte of flags.
            const uint16_t flags = opcode == 24 ? read_word() : DRAW_MASKED;
            if (flags > 0xFF)
                vga_fault(Fault::VGA_FLAGS_TOO_WIDE, flags);
            if (VgaSprite* sprite = current_sprite()) {
                sprite->image = image;
                sprite->x = static_cast<int16_t>(sprite->x + dx);
                sprite->y = static_cast<int16_t>(sprite->y + dy);
                sprite->flags = static_cast<uint8_t>(flags);
                sprite->window |= WINDOW_CHANGED;
            }
            break;
        }
        case 23: { // SET_PRIORITY: the array stays sorted, because array order is
                   // draw order (vga.cpp:999)
            const uint16_t priority = read_word();
            VgaSprite* sprite = current_sprite();
            if (!sprite)
                break;
            VgaSprite moved = *sprite;
            moved.priority = priority;
            moved.window |= WINDOW_CHANGED;
            uint8_t at = static_cast<uint8_t>(sprite - sprites_);
            while (at > 0 && priority < sprites_[at - 1].priority) {
                sprites_[at] = sprites_[at - 1];
                --at;
            }
            while (sprites_[at + 1].id != 0 && priority >= sprites_[at + 1].priority) {
                sprites_[at] = sprites_[at + 1];
                ++at;
            }
            sprites_[at] = moved;
            break;
        }
        case 60: { // STOP_ANIMATE
            const uint16_t sprite_id = read_word();
            stop_animation(static_cast<uint8_t>(sprite_id / SPRITES_PER_ZONE), sprite_id);
            break;
        }

        // --- the pathfinder: routes declared by one script, walked by another
        case 11: // CLEAR_PATHFIND_ARRAY
            for (uint8_t slot = 0; slot < PATH_SLOTS; ++slot)
                path_[slot] = NOWHERE;
            break;
        case 17: { // SET_PATHFIND_ITEM: the route is the list left in the script
            const uint16_t slot = read_word();
            const Place route = pc_;
            while (far_read16(pc_) != PATH_END)
                pc_ += 4;
            pc_ += 2;
            if (slot - 1u < PATH_SLOTS)
                path_[slot - 1] = route;
            else
                vga_fault(Fault::VGA_PATH_SLOT_RANGE, slot);
            break;
        }
        case 48: { // COMPUTE_YOFS: split each step of the route into two halves
            uint8_t route = static_cast<uint8_t>(read_var(PATH_ROUTE_VAR));
            // The engine's own workaround: a save can name a route that was never
            // declared, and the first one is near enough to keep walking (vga.cpp).
            if (route - 1u >= PATH_SLOTS || path_[route - 1] == NOWHERE)
                route = 1;
            Place at = path_[route - 1];
            if (at == NOWHERE) {
                vga_fault(Fault::VGA_NO_ROUTE, route);
                break;
            }
            at += 4u * read_var(PATH_INDEX_VAR) + 2;

            int16_t count = static_cast<int16_t>(read_var(PATH_COUNT_VAR));
            int8_t step = 4;
            if (count < 0) {
                count = static_cast<int16_t>(-count);
                step = -4;
            }
            // Wide enough to step past the last variable rather than wrapping to
            // nought: the pair written here is result and result + 1, so a uint8_t
            // that reaches 254 writes 254 and 255 and then wraps, and the check below
            // never fires. NUM_VARS being 256 is what exposed that.
            uint16_t result = PATH_FIRST_RESULT;
            for (; count > 0; --count) {
                // Both sides unsigned before the subtraction: on the target int is 16
                // bits, so a signed operand here converts rather than promotes.
                const uint16_t from = far_read16(at);
                at += static_cast<Place>(step); // sign-extended, so a back step wraps
                const int16_t span = static_cast<int16_t>(far_read16(at) - from);
                if (result + 1u >= NUM_VARS) {
                    vga_fault(Fault::VGA_PATH_OVERRAN, result);
                    break;
                }
                script_->set_variable(static_cast<uint8_t>(result), static_cast<int16_t>(span / 2));
                script_->set_variable(
                    static_cast<uint8_t>(result + 1), static_cast<int16_t>(span - span / 2));
                result = static_cast<uint16_t>(result + 2);
            }
            break;
        }

        // --- variables and bits, shared with the script VM
        case 32: { // COPY_VAR
            const uint16_t value = read_var(read_word());
            write_var(read_word(), value);
            break;
        }
        case 39: { // SET_VAR
            const uint16_t number = read_word();
            write_var(number, read_word());
            break;
        }
        case 40:   // ADD_VAR
        case 41: { // SUB_VAR
            const uint16_t number = read_word();
            const uint16_t by = read_word();
            write_var(number,
                static_cast<uint16_t>(
                    opcode == 40 ? read_var(number) + by : read_var(number) - by));
            break;
        }
        case 47: { // ADD_VAR_F
            const uint16_t number = read_word();
            const uint16_t by = read_var(read_word());
            write_var(number, static_cast<uint16_t>(read_var(number) + by));
            break;
        }
        case 49: // SET_BIT
        case 50: // CLEAR_BIT
            script_->set_bit(read_word(), opcode == 49);
            break;

        // --- control flow
        case 12: { // DELAY: come back to the next instruction in so many frames
            const uint16_t frames = read_var_or_word();
            add_event(static_cast<int16_t>(frames * frame_count_ + VGA_BASE_DELAY),
                VgaEvent::ANIMATE_EVENT,
                pc_,
                current_id_,
                current_zone_);
            yielded_ = true;
            break;
        }
        case 42: { // DELAY_IF_NOT_EQ: come back to the whole instruction next frame
            // ScummVM resumes at `_vcPtr - 4`, which with a two-byte opcode is the
            // first operand rather than the opcode. This resumes at the instruction,
            // which is what the name means; the opcode occurs nowhere in the release,
            // so the difference is not one the data can settle.
            const Place again = pc_ - 2;
            const uint16_t value = read_var(read_word());
            if (value != read_word()) {
                add_event(static_cast<int16_t>(frame_count_ + 1),
                    VgaEvent::ANIMATE_EVENT,
                    again,
                    current_id_,
                    current_zone_);
                yielded_ = true;
            }
            break;
        }
        case 15: { // SYNC: release everything waiting on this id
            // Through send_sync, which records the sync as sent (vc15_sync,
            // vga.cpp:783). Omitting that record causes syncs sent by animation
            // scripts before the main script asks to be lost, stalling the wait.
            send_sync(read_word());
            break;
        }
        case 16: // WAIT_SYNC
            park(wait_sync_, read_word());
            yielded_ = true;
            break;
        case 18: { // JUMP_REL, from the end of its own operand
            const int16_t offset = static_cast<int16_t>(read_word());
            pc_ += static_cast<Place>(offset);
            break;
        }
        case 20: { // SET_REPEAT: the counter is written into the script itself, and
                   // little-endian, in a stream that is otherwise big-endian
            // Sequenced: read_word advances pc_, and the counter goes where it lands.
            const uint16_t count = read_word();
            far_write16_le(pc_, count);
            pc_ += 2;
            break;
        }
        case 21: { // END_REPEAT: back to just past SET_REPEAT while its count lasts
            const int16_t offset = static_cast<int16_t>(read_word());
            const Place counter = pc_ + static_cast<Place>(offset) + 4;
            const uint16_t left = far_read16_le(counter);
            if (left != 0) {
                far_write16_le(counter, static_cast<uint16_t>(left - 1));
                pc_ = counter + 2;
            }
            break;
        }
        case 25: // HALT_SPRITE
            halt_sprite();
            break;
        case 27: // RESET: every sprite, every rendezvous, every timer but the
                 // redraw heartbeat (vga.cpp:1124)
            for (uint8_t at = 0; at < MAX_SPRITES; ++at)
                sprites_[at] = VgaSprite();
            for (uint8_t at = 0; at < MAX_SLEEPERS; ++at)
                wait_sync_[at] = wait_end_[at] = on_stop_[at] = VgaSleeper();
            for (uint8_t at = 0; at < MAX_TIMERS && timers_[at].delay;)
                if (timers_[at].type == VgaEvent::ANIMATE_INT)
                    ++at;
                else
                    delete_event(at);
            // Not a yield: vc27_resetSprite never touches the script pointer
            // (vga.cpp:1124). RESET occurs 192 times in the release and is never the
            // last instruction -- SET_PALETTE or DRAW follows it 180 of those times --
            // so ending the script here loses a room its backdrop.
            break;
        case 29: // STOP_ALL_SOUNDS
            vga_stop_sounds();
            break;
        // The engine counts hides: SET_MOUSE_OFF makes it 200 and SET_MOUSE_ON one
        // then none, as MOUSE_ON's own zero does, so shown or not is all of it.
        case 33: // SET_MOUSE_ON
            vga_mouse(true);
            break;
        case 34: // SET_MOUSE_OFF
            vga_mouse(false);
            break;
        case 52: // PLAY_SOUND: the talkie plays the word as an effect id
            // (vc52_playSound, vga_e2.cpp:170-193)
            vga_effect(read_word());
            break;
        case 30: // SET_FRAME_RATE
            frame_count_ = read_word();
            break;
        case 4: // FADE_IN, a dummy in the engine too (vga.cpp:445)
            pc_ += VIDEO_PARAM_LEN[4];
            break;
        case 51: // ENABLE_BOX
            vga_enable_box(read_word());
            break;
        case 55: { // MOVE_BOX
            const uint16_t id = read_word();
            const auto dx = static_cast<int16_t>(read_word());
            vga_move_box(id, dx, static_cast<int16_t>(read_word()));
            break;
        }

        default:
            // The operands go even when the opcode does not, or everything after it
            // decodes as rubbish. Every opcode that reaches here has a length the
            // table states correctly; the one it gets wrong, SET_PATHFIND_ITEM, is
            // installed and handled above.
            pc_ += VIDEO_PARAM_LEN[opcode];
            ++unimplemented_;
            vga_unimplemented(opcode);
            break;
    }
}

} // namespace agos
