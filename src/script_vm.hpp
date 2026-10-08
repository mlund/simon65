// SPDX-License-Identifier: GPL-3.0-or-later

// The main script interpreter: the AND-chain, the operand forms, and the
// opcodes that need nothing outside the VM.

#pragma once

#include "fault.hpp"
#include "gamedb.hpp"
#include "tables.hpp"

#include <stdint.h>

#ifdef __mos__
#include "banknumbers.hpp"

#include <mapper.h>
/// Kept in the dispatch's bank when the optimiser declines to inline it,
/// rather than landing in the fixed region (banks.hpp).
#define SCRIPT_BANKED CODE_BANK(AGOS_SCRIPT_BANK)
#else
#define SCRIPT_BANKED
#endif

namespace agos {

/// One more than ScummVM's 255 (agos.cpp:823): a byte operand names
/// variable 255, which indexed a 255-element array one past its end.
inline constexpr uint16_t NUM_VARS = 256;
/// ScummVM allocates 50 and 20. The release never names an item-store slot
/// above 3 or a VGA slot above 4, and both opcodes already fault past the end,
/// so these follow PATH_SLOTS and are sized to the game rather than the engine.
inline constexpr uint8_t ITEM_STORE = 16;
inline constexpr uint8_t OBJECT_SLOTS = 16;
/// Sixteen, because the highest box SET_SHORT_TEXT or SET_LONG_TEXT names
/// over the release is fifteen. Every use is range-checked, so a box past the
/// end is a write dropped rather than memory overwritten.
inline constexpr uint8_t TEXT_BOXES = 16;

/// A sprite this high starts something new, so a sync already sent cannot
/// belong to it and is forgotten (script_s1.cpp:296).
inline constexpr uint16_t ANIMATE_FORGETS_SYNC = 400;

/// Speech, which is waited for on its own terms: the already-sent rule is not
/// applied to it (script.cpp:1070).
inline constexpr uint16_t SPEECH_SYNC = 200;

/// The verb a conversation's choice answers to (script_ww.cpp:306).
inline constexpr uint16_t TEXT_VERB = 208;

/// playSpeech's no-voice line and the pause it plays: zone 1's sprite 130 in
/// window 4, timed by variable 100, once until bit 14 is cleared; bit 28
/// stops it (AGOSEngine_Simon1::playSpeech, res_snd.cpp:55).
inline constexpr uint16_t NO_VOICE = 9999;
inline constexpr uint16_t PAUSED_BIT = 14, NO_PAUSE_BIT = 28;
inline constexpr uint8_t PAUSE_TICKS_VAR = 100;
inline constexpr int16_t PAUSE_TICKS = 15;
inline constexpr uint16_t PAUSE_SPRITE = 130;
inline constexpr uint8_t PAUSE_WINDOW = 4;
inline constexpr uint8_t MAX_RECURSION = 40; // subroutine.cpp:544

/// 256 bits. ScummVM allocates 128 words and uses 16 (agos.cpp:819); going
/// past the 16 is a fault here rather than silent corruption of what follows.
inline constexpr uint8_t BIT_WORDS = 16;

/// The player, built by createPlayer rather than read from the file
/// (items.cpp:77). Item 0 is the null item.
inline constexpr uint16_t PLAYER_ITEM = 1;

/// A variable operand is a word in this range holding the variable's number
/// (getVarOrWord, script.cpp:924).
inline constexpr uint16_t VARIABLE_BASE = 30000;
inline constexpr uint16_t VARIABLE_TOP = 30512;

/// Script return codes (runScript, script.cpp:979).
inline constexpr int16_t SCRIPT_CONTINUE = 0;
inline constexpr int16_t SCRIPT_DONE = 1;
inline constexpr int16_t SCRIPT_RESCAN = -10;

/// What a script asks for that the VM does not own: the screen, sound, the
/// mouse, the animation VM.
///
/// Every one is a loud failure until the tier that provides it arrives, and
/// none is quietly a no-op -- a script that silently does nothing runs on and
/// corrupts state a long way from the cause.
void script_unimplemented(uint8_t opcode);
/// A line of subroutine @p id about to run, at @p offset bytes into it.
void script_line(uint16_t id, uint16_t offset);

/// Where a line of speech goes, and the line itself.
///
/// The script says the place first (161) and the words after (162), and the
/// two arrive apart because that is how the original reads them: a location
/// is set once and several lines are said at it.
void script_text_box(uint8_t which, int16_t x, uint8_t y, uint16_t width);
void script_text_msg(uint8_t which, uint8_t colour, uint16_t string, uint16_t speech);

/// The room taken down to black as it is left (187).
void script_fade_to_black();

/// An effect played (163), by its place in the sound set chosen (185).
void script_effect(uint16_t id);
void script_sound_set(uint16_t set);

/// The text windows: defined (101), picked (102), cleared (103), inked (160)
/// and written into (63). Six windows in the release and two inks, so what is
/// behind these is a table rather than a window system.
void script_window(
    uint8_t which, uint8_t x, uint16_t y, uint8_t cells, uint8_t rows, uint8_t flags = 0);
void script_use_window(uint8_t which);
void script_clear_window();
void script_ink(uint8_t colour);
void script_show_string(uint16_t string);

/// What the script asks the animation VM for: a room's picture (96), a zone
/// brought in (97), a sprite started (98) or all of them stopped (100), and
/// the two halves of a rendezvous (119, 120).
void script_picture(uint8_t zone, uint16_t image, uint8_t window);
void script_load_zone(uint8_t zone);
void script_animate(
    uint16_t window, uint8_t zone, uint16_t sprite, int16_t x, int16_t y, uint8_t palette);
void script_kill_animate();
/// LOAD_BEARD or UNLOAD_BEARD: zone 11's pixels with the beard, or without.
void script_beard(bool on);
/// HALT_ANIMATION or RESTART_ANIMATION: the animation clock stopped or let go.
void script_halt_animation(bool halted);
/// STOP_ANIMATE: one sprite and its script stopped (vc60_stopAnimation).
void script_stop_animate(uint16_t sprite);
/// Which subroutine the script has just entered, for the report.
void script_wait_sync(uint16_t ident);
void script_sync(uint16_t ident);

/// A tune, by the number the game knows it as (127).
void script_play_tune(uint16_t music, uint16_t track);

/// A box a click is tested against (107), and the pointer going away (181).
/// @p flags is ADD_BOX's thousands, or TEXT_CHOICE with a conversation
/// choice's slot (65).
void script_add_box(uint16_t id,
    uint16_t x,
    uint16_t y,
    uint16_t w,
    uint16_t h,
    uint8_t flags,
    uint16_t verb,
    uint16_t item);
/// The route point nearest a place (178): the route's number in the high
/// byte, the point's in the low.
[[nodiscard]] uint16_t script_route_point(uint16_t x, uint16_t y);
/// Marks ADD_TEXT_BOX's flags: ADD_BOX's thousands never reach it.
inline constexpr uint8_t TEXT_CHOICE = 0x80;
/// The box opcodes, which script_box passes on as they are.
enum BoxOpcode : uint8_t {
    DEL_BOX = 108,
    ENABLE_BOX = 109,
    DISABLE_BOX = 110,
    IS_BOX = 142,
    MOVE_BOX = 0xFF, // the video VM's (vc55), so not an opcode of this one
};
/// A box deleted, enabled or disabled, named by the opcode; or asked whether
/// it is defined and alive.
[[nodiscard]] bool script_box(uint16_t id, uint8_t opcode);
void script_mouse_off();
void script_mouse_on();

#ifdef __mos__
/// The script dispatch, behind a door of its own for the same reason the
/// animation dispatch is: 188 opcodes of it will not fit the fixed region
/// beside everything else, and it has one caller. Its bank is in banks.hpp.
extern "C" void script_execute_banked();

#endif

/// Bring the file holding this subroutine into the heap, as getSubroutineByID
/// does on a miss (subroutine.cpp:209). Which file it is comes from tbllist;
/// reading it is the host's business, because where the data lives is what
/// changes between the host and the machine. False if there is no such
/// subroutine anywhere.
bool script_load_subroutine(uint16_t subroutine_id);

/// An item gained or lost a child. The engine redraws any icon window showing
/// that item (itemChildrenChanged, items.cpp:292).
void script_items_changed(uint16_t item);
/// The icons of what @p item holds, in text window @p window (114).
void script_do_icons(uint16_t item, uint8_t window);
/// The player's pick of a second item (164): waits for a click on a box with
/// an item and returns that item.
[[nodiscard]] uint16_t script_pick_object();

/// A rescan's pause: the world turns if a frame is due (delay(0) in
/// startSubroutine, subroutine.cpp:620-624). Sub 181 rescans until an
/// animation sets bit 50, which never happens if the world stands still.
void script_rescan();

/// The game saved to and loaded from its one slot (132, 133). The CD32 build
/// has a single slot and no dialog (o_saveUserGame, script.cpp:805-831).
void script_save_game();
void script_load_game();

/// The interpreter.
///
/// It owns the variables, the bit flags, the item store and the script's own
/// control flow, and it reaches the item tree through GameDb and its code
/// through SubroutineHeap. Everything else leaves through the two hooks above,
/// so the whole thing can be run and tested with nothing else attached.
class ScriptVm {
  public:
    void reset(GameDb& db, SubroutineHeap& heap) {
        db_ = &db;
        heap_ = &heap;
        for (uint16_t i = 0; i < NUM_VARS; ++i)
            variables_[i] = 0;
        for (uint8_t i = 0; i < BIT_WORDS; ++i)
            bits_[i] = bits2_[i] = 0;
        for (uint8_t i = 0; i < ITEM_STORE; ++i)
            item_store_[i] = NO_ITEM;
        subject_ = object_ = NO_ITEM;
        chance_modifier_ = 0;
        depth_ = 0;
        unimplemented_ = 0;
    }

    /// Run one subroutine to completion, as o_process does. A subroutine that is
    /// not in the heap sends the host to fetch its file first.
    [[gnu::always_inline]] int16_t start(uint16_t subroutine_id) {
        Subroutine sub = heap_->find(subroutine_id);
        if (!sub.valid() && script_load_subroutine(subroutine_id))
            sub = heap_->find(subroutine_id);
        if (!sub.valid()) {
            script_fault(Fault::NO_SUBROUTINE, subroutine_id);
            return SCRIPT_CONTINUE;
        }
        return start_subroutine(sub);
    }

    /// o_end quits the game outright (script.cpp:415); with no game loop here it
    /// stops the run instead.
    [[nodiscard]] bool quit() const {
        return quit_;
    }

    [[nodiscard]] int16_t variable(uint8_t number) const {
        return variables_[number];
    }
    void set_variable(uint8_t number, int16_t value) {
        variables_[number] = value;
    }
    [[nodiscard]] bool bit(uint16_t number) const {
        return read_bit(bits_, number);
    }
    void set_bit(uint16_t number, bool on) {
        write_bit(bits_, number, on);
    }

    [[nodiscard]] uint16_t subject() const {
        return subject_;
    }
    [[nodiscard]] uint16_t object() const {
        return object_;
    }

    /// How many opcodes fell through to the hook. Zero once every tier is in.
    [[nodiscard]] uint16_t unimplemented() const {
        return unimplemented_;
    }

    /// End the script chain that is running, the way the engine does, and let
    /// go of any wait it is in.
    ///
    /// Set once, it makes every script on the way out return 1 and stays set
    /// until a top-level driver clears it (runScript, script.cpp:999;
    /// invokeTimeEvent, event.cpp:119-126). That is what unwinds a cutscene:
    /// without it the interrupted script carries on, reaches its next wait, and
    /// the skip nests the whole interpreter inside itself.
    void unwind() {
        wait_for_ = 0;
        return1_ = true;
    }
    [[nodiscard]] bool returning() const {
        return return1_;
    }
    void clear_returning() {
        return1_ = false;
    }

    /// The subroutine running now and how many have run: with no screen to
    /// watch, this is how a stalled script says where it stalled.
    [[nodiscard]] uint16_t in_sub() const {
        return in_sub_;
    }
    [[nodiscard]] uint16_t stopped_on() const {
        return stopped_on_;
    }

    /// What the trampoline came in to run.
    void execute_pending() {
        execute(pending_);
    }

    /// The animation VM reads and writes the same variables and bits, tests the
    /// items the script put in the VGA slots, and releases the script's wait.
    [[nodiscard]] uint16_t* bits() {
        return bits_;
    }
    [[nodiscard]] GameDb& db() const {
        return *db_;
    }
    [[nodiscard]] uint16_t vga_object(uint8_t slot) const {
        return slot < OBJECT_SLOTS ? objects_[slot] : NO_ITEM;
    }
    /// A text box's name, by its slot (SET_SHORT_TEXT); nought when unset.
    [[nodiscard]] uint16_t short_text(uint8_t slot) const {
        return slot < TEXT_BOXES ? short_text_[slot] : 0;
    }

    /// Start waiting for sync @p ident; false if it has already gone past,
    /// which counts as arrived (waitForSync, script.cpp:1069). This release's
    /// scripts sync before the main script asks, and without this the ask is
    /// for ever. The speech sync is always waited for.
    [[nodiscard]] bool wait_for_sync(uint16_t ident) {
        if (ident != SPEECH_SYNC) {
            const uint16_t sent = last_sync_;
            last_sync_ = 0;
            if (sent == ident)
                return false;
        }
        wait_for_ = ident;
        return true;
    }
    /// Whether the wait is still on: the animation VM has not synced.
    [[nodiscard]] bool waiting() const {
        return wait_for_ != 0;
    }
    /// The wait given up, the script left to run on.
    void stop_waiting() {
        wait_for_ = 0;
    }
    /// The animation VM's sync @p ident: kept for a wait not yet started, and
    /// the wait on it released (vga.cpp:805).
    void synced(uint16_t ident) {
        last_sync_ = ident;
        if (ident == wait_for_)
            wait_for_ = 0;
    }
    /// Every sync sent forgotten, as a sprite reset does (vga.cpp:1088).
    void forget_syncs() {
        last_sync_ = 0;
    }
    [[nodiscard]] uint16_t player_parent() const {
        return db_->item(PLAYER_ITEM).parent();
    }

    /// Subroutines the script has asked to run later (76), and the clock they
    /// are against.
    ///
    /// Seconds, which is what the engine counts in -- getTime() is the
    /// millisecond clock divided by a thousand (event.cpp:43). The release asks
    /// for 58 of these over the whole game, for 21 distinct subroutines, and
    /// mostly one to three seconds out, so sixteen live at once is generous.
    static constexpr uint8_t TIMEOUTS = 16;

    void add_timeout(uint16_t seconds, uint16_t subroutine) {
        for (uint8_t i = 0; i < TIMEOUTS; ++i)
            if (waiting_[i] == 0) {
                waiting_[i] = subroutine;
                due_[i] = static_cast<uint32_t>(clock_ + seconds);
                return;
            }
        script_fault(Fault::TIMEOUTS_FULL, subroutine);
    }

    /// Move the clock on and run whatever has come due, earliest first.
    ///
    /// Called from the loop and never from inside a script: a subroutine that
    /// runs here can schedule another, which is why the scan starts again
    /// rather than walking a list it may have changed (kickoffTimeEvents,
    /// event.cpp:139).
    uint8_t clock(uint32_t seconds) {
        clock_ = seconds;
        uint8_t ran = 0;
        for (;;) {
            uint8_t soonest = TIMEOUTS;
            for (uint8_t i = 0; i < TIMEOUTS; ++i)
                if (waiting_[i] != 0 && due_[i] <= clock_ &&
                    (soonest == TIMEOUTS || due_[i] < due_[soonest]))
                    soonest = i;
            if (soonest == TIMEOUTS)
                return ran;
            if (return1_)
                return ran; // invokeTimeEvent, event.cpp:119
            const uint16_t subroutine = waiting_[soonest];
            waiting_[soonest] = 0; // forgotten before it runs, as the engine
            start(subroutine);     // forgets it before invoking
            return1_ = false;      // and clears it after, :126
            ++ran;
        }
    }

    [[nodiscard]] uint8_t timeouts() const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < TIMEOUTS; ++i)
            n = static_cast<uint8_t>(n + (waiting_[i] != 0 ? 1 : 0));
        return n;
    }

    /// A click: the verb a box names and the item it holds.
    ///
    /// Subroutine 0 is the one whose lines carry verb and noun, and its match
    /// is what turns a press into something happening; 100 runs after it, as
    /// the engine runs it (handleVerbClicked, verb.cpp:392-404). The nouns come
    /// from the item the box was defined with -- there is no second item until
    /// something can be dragged onto something else.
    void clicked(int16_t verb, uint16_t subject) {
        const Item item = db_->item(subject);
        // The box's item is the subject the verb's lines test (o_if1,
        // script.cpp:469): without it every item verb stopped at its first line.
        // No second item is picked yet, so no object (handleVerbClicked,
        // verb.cpp:352-372).
        subject_ = item.valid() ? subject : NO_ITEM;
        object_ = NO_ITEM;
        script_verb_ = verb;
        script_noun1_ = item.valid() ? static_cast<int16_t>(item.noun()) : int16_t{-1};
        script_noun2_ = -1;
        return1_ = false; // handleVerbClicked, verb.cpp:401
        start(0);
        return1_ = false;
        start(100);
        return1_ = false; // :408
        script_verb_ = -1;
        script_noun1_ = -1;
        script_noun2_ = -1;
    }

    /// A line's voice number, as playSpeech takes it (res_snd.cpp:55; the CD32
    /// runs without subtitles, agos.cpp:695-698). 9999 marks a line with no
    /// voice: the pause sprite runs once unless a line already did, and the
    /// next wait for sync 200 is let by -- nothing will send it.
    [[gnu::always_inline]] void voice_cue(uint16_t speech) {
        if (speech != NO_VOICE)
            return;
        if (!bit(PAUSED_BIT) && !bit(NO_PAUSE_BIT)) {
            set_bit(PAUSED_BIT, true);
            variables_[PAUSE_TICKS_VAR] = PAUSE_TICKS;
            script_animate(PAUSE_WINDOW, PAUSE_SPRITE / 100, PAUSE_SPRITE, 0, 0, 0);
            script_wait_sync(PAUSE_SPRITE);
        }
        skip_line_wait_ = true;
    }

    /// Deterministic, so a host run repeats. It is not ScummVM's generator, so a
    /// trace diverges wherever CHANCE or RANDOM is reached -- by design, since
    /// matching an unrelated generator would prove nothing.
    void seed(uint32_t value) {
        rng_ = value ? value : 1;
    }

  private:
    /// The save file is this VM's state, field for field.
    friend class SaveGame;

    // ------------------------------------------------------------- operands

    uint8_t next_byte() {
        return far_read8(code_++);
    }

    int16_t next_word() {
        const int16_t value = static_cast<int16_t>(far_read16(code_));
        code_ += 2;
        return value;
    }

    /// getVarOrByte: 255 escapes to a variable number in the following byte.
    uint16_t var_or_byte() {
        const uint8_t value = next_byte();
        return value != 0xFF ? value : read_variable(next_byte());
    }

    /// getVarOrWord: a word, or a variable's contents when it names one.
    uint16_t var_or_word() {
        const uint16_t value = far_read16(code_);
        code_ += 2;
        return value >= VARIABLE_BASE && value < VARIABLE_TOP
            ? read_variable(static_cast<uint8_t>(value - VARIABLE_BASE))
            : value;
    }

    uint16_t next_var_contents() {
        return read_variable(var_or_byte());
    }
    void write_next_var_contents(uint16_t value) {
        write_variable(var_or_byte(), value);
    }

    [[nodiscard]] uint16_t read_variable(uint16_t number) const {
        if (number >= NUM_VARS) {
            script_fault(Fault::VARIABLE_RANGE, number);
            return 0;
        }
        return static_cast<uint16_t>(variables_[number]);
    }
    void write_variable(uint16_t number, uint16_t value) {
        if (number >= NUM_VARS)
            script_fault(Fault::VARIABLE_RANGE, number);
        else
            variables_[number] = static_cast<int16_t>(value);
    }

    /// getNextItemID: the pseudo-items, then a plain index. ACTOR is nil here,
    /// where getNextItemPtr would have errored (items.cpp:224, :250).
    uint16_t next_item_id() {
        const int16_t value = next_word();
        switch (value) {
            case -1:
                return subject_;
            case -3:
                return object_;
            case -5:
                return PLAYER_ITEM;
            case -7:
                return NO_ITEM;
            case -9:
                return db_->item(PLAYER_ITEM).parent();
            default:
                return static_cast<uint16_t>(value);
        }
    }

    /// An item to work on. The null item has no record -- and neither does the
    /// ACTOR pseudo-item, which `next_item_id` hands back as nil -- so reaching
    /// one is a fault, and the caller gets a scratch record rather than a null
    /// pointer. One branch here rather than a check in each of the seventeen
    /// opcodes that dereference what they are given.
    [[nodiscard]] Item item_of(uint16_t id) {
        const Item item = db_->item(id);
        if (item.valid())
            return item;
        script_fault(Fault::ITEM_MISSING, id);
        return Item(scratch_);
    }
    Item next_item() {
        return item_of(next_item_id());
    }

    // ---------------------------------------------------------------- state

    [[nodiscard]] static bool read_bit(const uint16_t* array, uint16_t number) {
        if (number >= BIT_WORDS * 16) {
            script_fault(Fault::BIT_RANGE, number);
            return false;
        }
        return (array[number / 16] >> (number & 15)) & 1;
    }
    static void write_bit(uint16_t* array, uint16_t number, bool on) {
        if (number >= BIT_WORDS * 16) {
            script_fault(Fault::BIT_RANGE, number);
            return;
        }
        const uint16_t mask = static_cast<uint16_t>(1U << (number & 15));
        uint16_t& word = array[number / 16];
        word = static_cast<uint16_t>(on ? word | mask : word & ~mask);
    }

    uint16_t random(uint16_t below) {
        // xorshift32. Small, and it repeats, which is what a host test needs.
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return below ? static_cast<uint16_t>(rng_ % below) : 0;
    }

    // ------------------------------------------------------------ the tree

    /// setItemParent: unlink from the old parent's chain, link at the head of the
    /// new one's (setItemParent, items.cpp:271-290), each parent told so that
    /// icons showing it are redrawn.
    SCRIPT_BANKED void set_item_parent(uint16_t id, uint16_t parent_id, bool notify) {
        if (id == parent_id) {
            script_fault(Fault::ITEM_ITS_OWN_PARENT, id);
            return;
        }
        const Item item = item_of(id);
        if (!item.valid()) {
            script_fault(Fault::MOVING_NULL_ITEM, id);
            return;
        }
        const uint16_t old_parent = item.parent();
        unlink_item(id);
        if (notify)
            script_items_changed(old_parent);
        const Item parent = item_of(parent_id);
        item.set_parent(parent_id);
        if (parent.valid()) {
            item.set_next(parent.child());
            parent.set_child(id);
        } else {
            item.set_next(NO_ITEM);
        }
        if (notify)
            script_items_changed(parent_id);
    }

    void unlink_item(uint16_t id) {
        const Item item = item_of(id);
        const uint16_t parent_id = item.parent();
        if (parent_id == NO_ITEM)
            return;
        const Item parent = item_of(parent_id);
        if (!parent.valid()) {
            script_fault(Fault::ITEM_OFF_NULL_ITEM, id);
            return;
        }
        if (parent.child() == id) {
            parent.set_child(item.next());
            item.set_parent(NO_ITEM);
            item.set_next(NO_ITEM);
            return;
        }
        for (uint16_t at = parent.child(); at != NO_ITEM;) {
            const Item sibling = item_of(at);
            if (!sibling.valid())
                break;
            if (sibling.next() == id) {
                sibling.set_next(item.next());
                item.set_parent(NO_ITEM);
                item.set_next(NO_ITEM);
                return;
            }
            at = sibling.next();
        }
        script_fault(Fault::ITEM_NOT_A_CHILD, id);
    }

    /// synchChain copies a state change along kChainType children (items.cpp).
    /// No item in this release has one -- 92 carry a room, 90 an object, 10
    /// nothing -- so there is nothing to copy it to.
    static void synch_chain(const Item&) {
    }

    // ------------------------------------------------------------- dispatch

    void execute(uint8_t opcode);

    /// runScript: fetch, invert, dispatch, and stop as soon as a condition comes
    /// out the other way (script.cpp:979).
    int16_t run_script();

    /// startSubroutine: run each line the matcher lets through, with the caller's
    /// position saved around it. There is no stack in the bytecode, so this is
    /// the one that recurses (subroutine.cpp:523).
    int16_t start_subroutine(const Subroutine& sub);

    [[nodiscard]] bool line_matches(const Subroutine& sub, Place line) const;

    GameDb* db_ = nullptr;
    SubroutineHeap* heap_ = nullptr;

    Place code_ = NOWHERE;
    Subroutine current_table_;
    Place table_end_ = NOWHERE;

    int16_t variables_[NUM_VARS] = {};
    uint16_t bits_[BIT_WORDS] = {};
    uint16_t bits2_[BIT_WORDS] = {};
    uint16_t item_store_[ITEM_STORE] = {};
    uint16_t objects_[OBJECT_SLOTS] = {};
    uint16_t short_text_[TEXT_BOXES] = {};
    uint16_t long_text_[TEXT_BOXES] = {};
    uint16_t long_sound_[TEXT_BOXES] = {};

    uint16_t wait_for_ = 0;
    uint16_t last_sync_ = 0;
    bool return1_ = false;
    /// _skipVgaWait: a line with no voice lets the next wait for sync 200 by.
    bool skip_line_wait_ = false;
    uint16_t in_sub_ = 0;
    uint16_t stopped_on_ = 0;
    uint16_t subject_ = NO_ITEM;
    uint16_t object_ = NO_ITEM;
    int16_t chance_modifier_ = 0;
    uint32_t rng_ = 1;

    /// Where a fault's reads and writes go, so they cannot reach page zero.
    uint8_t scratch_[PLAYER_RECORD] = {};

    uint8_t pending_ = 0;
    bool condition_ = true;
    bool quit_ = false;
    int16_t return_ = 0;
    uint8_t depth_ = 0;
    uint16_t unimplemented_ = 0;

    uint32_t clock_ = 0;
    uint32_t due_[TIMEOUTS] = {};
    uint16_t waiting_[TIMEOUTS] = {};

    // The verb and noun a matching line is tested against. Only subroutine 0 has
    // them, and only input sets them, so a headless run leaves them unset.
    int16_t script_verb_ = -1;
    int16_t script_noun1_ = -1;
    int16_t script_noun2_ = -1;
};

// ---------------------------------------------------------------- definitions

inline int16_t ScriptVm::run_script() {
    bool inverted;
    do {
        // Checked before the opcode, not after: the chain unwinds from wherever
        // it is (script.cpp:999), and the flag is left set so that every script
        // above this one returns as well.
        if (return1_)
            return 1;

        // A line ends at its own LINE_END, and the heap puts subroutines back to
        // back: a line that runs past its end is executing the next subroutine's
        // opcodes from the middle, which nothing else would ever notice.
        if (code_ >= table_end_) {
            script_fault(Fault::LINE_RAN_PAST, current_table_.id());
            return SCRIPT_CONTINUE;
        }
        uint8_t opcode = next_byte();
        if (opcode == LINE_END)
            return SCRIPT_CONTINUE;

        // Opcode 0 inverts the next one; it is a prefix, not an instruction, and
        // the only reason o_invalid exists is that it is never dispatched.
        inverted = opcode == 0;
        if (inverted) {
            opcode = next_byte();
            if (opcode == LINE_END)
                return SCRIPT_CONTINUE;
        }

        condition_ = true;
        return_ = 0;
        if (opcode >= SCRIPT_OPCODES) {
            script_fault(Fault::OPCODE_RANGE, opcode);
            return SCRIPT_CONTINUE;
        }
#ifdef __mos__
        pending_ = opcode;
        banked_reenter<script_execute_banked>(AGOS_SCRIPT_BANK);
#else
        execute(opcode);
#endif
        // A line is an implicit AND-chain: it stops the moment a condition comes
        // out the other way from what the invert prefix asked for.
    } while (condition_ != inverted && return_ == 0 && !quit_);
    return return_;
}

inline bool ScriptVm::line_matches(const Subroutine& sub, Place line) const {
    // checkIfToRunSubroutineLine, subroutine.cpp:644. Only subroutine 0 matches;
    // -1 is a wildcard and -2 matches only the unset case.
    if (sub.id() != MATCHING_SUBROUTINE)
        return true;
    const int16_t verb = sub.verb(line);
    const int16_t noun1 = sub.noun1(line);
    const int16_t noun2 = sub.noun2(line);
    if (verb != -1 && verb != script_verb_)
        return false;
    if (noun1 != -1 && noun1 != script_noun1_)
        return false;
    if (noun2 != -1 && noun2 != script_noun2_ && (noun2 != -2 || script_noun2_ != -1))
        return false;
    return true;
}

inline int16_t ScriptVm::start_subroutine(const Subroutine& sub) {
    in_sub_ = sub.id();
    if (++depth_ > MAX_RECURSION) {
        script_fault(Fault::RECURSION_DEEP, depth_);
        --depth_;
        return SCRIPT_CONTINUE;
    }

    const Place old_code = code_;
    const Subroutine old_table = current_table_;
    const Place old_end = table_end_;
    // The caller's line goes on testing its own condition and return: the
    // engine keeps both per depth (_runScriptCondition, script.cpp:38-44), and
    // a callee's last failed test would otherwise end the caller's line.
    const bool old_condition = condition_;
    const int16_t old_return = return_;

    // The engine also saves _classLine/_classMask/_classMode1/_classMode2 here.
    // Only oe2_doClass sets those, and that opcode does not occur in this
    // release's data, so the class rescan below them is dead here.

    current_table_ = sub;
    table_end_ = sub.end();
    int16_t result = -1;
    // The id and the length are two far words that cannot change under the loop,
    // and reading them back per line costs eight far calls for nothing -- per
    // opcode, where the guard in run_script reads it, eight times as many.
    const Place end = table_end_;
    const uint16_t id = sub.id();
    const bool matching = id == MATCHING_SUBROUTINE;
    for (Place line = sub.begin(); line < end;) {
        const Place opcodes = matching ? line + MATCH_BYTES : line;
        if (!matching || line_matches(sub, line)) {
            script_line(id, static_cast<uint16_t>(line - sub.begin()));
            code_ = opcodes;
            result = run_script();
        }
        // From the start of the line, not from code_: a line stops wherever its
        // chain of conditions gave out, which is rarely at its end.
        line = line_end(opcodes);
        // RESCAN starts the subroutine again from its first line, which is how
        // a script loops (subroutine.cpp:620-624): sub 5 deletes a room's boxes
        // one a pass.
        if (result == SCRIPT_RESCAN && !quit_) {
            line = sub.begin();
            result = -1;
            continue;
        }
        if (result != 0 || quit_)
            break;
    }

    // What ended the walk, for a run with no screen: nought is the subroutine's
    // last line, -1 is a subroutine none of whose lines matched, anything else
    // is the line that stopped it.
    stopped_on_ = static_cast<uint16_t>(result);
    code_ = old_code;
    current_table_ = old_table;
    table_end_ = old_end;
    condition_ = old_condition;
    return_ = old_return;
    --depth_;
    return result;
}

/// The opcodes that need nothing outside the VM: the conditions, the
/// arithmetic, the item tree, the bits and the script's own control flow.
/// Everything else leaves through script_unimplemented, loudly.
inline void ScriptVm::execute(uint8_t opcode) {
    switch (opcode) {
        // --- where things are
        case 1: { // AT: me's parent is
            const uint16_t parent = db_->item(PLAYER_ITEM).parent();
            condition_ = parent == next_item_id();
            break;
        }
        case 5: // CARRIED: parent is the player
            condition_ = next_item().parent() == PLAYER_ITEM;
            break;
        case 7: { // IS_AT
            const uint16_t parent = next_item().parent();
            condition_ = parent == next_item_id();
            break;
        }
        case 125: { // HERE: shares the player's parent
            const uint16_t parent = next_item().parent();
            condition_ = parent == db_->item(PLAYER_ITEM).parent();
            break;
        }
        case 80: { // IS: the same item
            const uint16_t left = next_item_id();
            condition_ = left == next_item_id();
            break;
        }

        // --- variables
        case 11:
            condition_ = next_var_contents() == 0;
            break;
        case 12:
            condition_ = next_var_contents() != 0;
            break;
        case 13: {
            const uint16_t left = next_var_contents();
            condition_ = left == var_or_word();
            break;
        }
        case 14: {
            const uint16_t left = next_var_contents();
            condition_ = left != var_or_word();
            break;
        }
        case 15: {
            const int16_t left = static_cast<int16_t>(next_var_contents());
            condition_ = left > static_cast<int16_t>(var_or_word());
            break;
        }
        case 16: {
            const int16_t left = static_cast<int16_t>(next_var_contents());
            condition_ = left < static_cast<int16_t>(var_or_word());
            break;
        }
        case 17: {
            const uint16_t left = next_var_contents();
            condition_ = left == next_var_contents();
            break;
        }
        case 18: {
            const uint16_t left = next_var_contents();
            condition_ = left != next_var_contents();
            break;
        }
        case 19: { // LT_F -- named the other way round in the engine's comment
            const int16_t left = static_cast<int16_t>(next_var_contents());
            condition_ = left < static_cast<int16_t>(next_var_contents());
            break;
        }
        case 20: {
            const int16_t left = static_cast<int16_t>(next_var_contents());
            condition_ = left > static_cast<int16_t>(next_var_contents());
            break;
        }
        case 36: { // COPY_VAR
            const uint16_t value = next_var_contents();
            write_next_var_contents(value);
            break;
        }
        case 41:
            write_next_var_contents(0);
            break;
        case 42: { // SET
            const uint16_t number = var_or_byte();
            write_variable(number, var_or_word());
            break;
        }
        case 43:   // ADD
        case 44: { // SUB
            const uint16_t number = var_or_byte();
            const uint16_t by = var_or_word();
            write_variable(number,
                static_cast<uint16_t>(
                    opcode == 43 ? read_variable(number) + by : read_variable(number) - by));
            break;
        }
        case 45:   // ADD_F
        case 46: { // SUB_F
            const uint16_t number = var_or_byte();
            const uint16_t by = next_var_contents();
            write_variable(number,
                static_cast<uint16_t>(
                    opcode == 45 ? read_variable(number) + by : read_variable(number) - by));
            break;
        }
        case 47: { // MUL
            const uint16_t number = var_or_byte();
            write_variable(number, static_cast<uint16_t>(read_variable(number) * var_or_word()));
            break;
        }
        case 48: { // DIV
            const uint16_t number = var_or_byte();
            const int16_t by = static_cast<int16_t>(var_or_word());
            if (by == 0) {
                script_fault(Fault::DIVIDE_BY_ZERO, number);
                break;
            }
            write_variable(
                number, static_cast<uint16_t>(static_cast<int16_t>(read_variable(number)) / by));
            break;
        }
        case 51: { // MOD
            const uint16_t number = var_or_byte();
            const int16_t by = static_cast<int16_t>(var_or_word());
            if (by == 0) {
                script_fault(Fault::MODULO_BY_ZERO, number);
                break;
            }
            write_variable(
                number, static_cast<uint16_t>(static_cast<int16_t>(read_variable(number)) % by));
            break;
        }
        case 53: { // RANDOM
            const uint16_t number = var_or_byte();
            write_variable(number, random(var_or_word()));
            break;
        }
        case 23: { // CHANCE, with the modifier that makes a run of luck less likely
            const int16_t chance = static_cast<int16_t>(var_or_word());
            if (chance == 0 || chance == 100) {
                condition_ = chance == 100;
                break;
            }
            const int16_t adjusted = static_cast<int16_t>(chance + chance_modifier_);
            if (adjusted <= 0) {
                chance_modifier_ = 0;
                condition_ = false;
            } else if (static_cast<int16_t>(random(100)) < adjusted) {
                chance_modifier_ =
                    chance_modifier_ <= 0 ? static_cast<int16_t>(chance_modifier_ - 5) : 0;
                condition_ = true;
            } else {
                chance_modifier_ =
                    chance_modifier_ >= 0 ? static_cast<int16_t>(chance_modifier_ + 5) : 0;
                condition_ = false;
            }
            break;
        }

        // --- items
        case 25:
            condition_ = next_item().is_room();
            break;
        case 26:
            condition_ = next_item().is_object();
            break;
        case 27: { // STATE_IS
            const uint16_t state = static_cast<uint16_t>(next_item().state());
            condition_ = state == var_or_word();
            break;
        }
        case 28: { // OBJECT_HAS_FLAG
            const Item item = next_item();
            const uint8_t bit = static_cast<uint8_t>(var_or_byte());
            condition_ = item.is_object() && item.object().has(bit);
            break;
        }
        case 56:   // OBJECT_SET_FLAG, only the bits above the packed values
        case 57: { // OBJECT_CLEAR_FLAG
            const Item item = next_item();
            const uint8_t bit = static_cast<uint8_t>(var_or_byte());
            if (item.is_object() && bit >= Object::FLAG_BITS)
                item.object().set_flag(bit, opcode == 56);
            break;
        }
        case 59: { // INC_STATE
            const Item item = next_item();
            if (item.state() <= 30000) {
                item.set_state(static_cast<int16_t>(item.state() + 1));
                synch_chain(item);
            }
            break;
        }
        case 60: { // DEC_STATE
            const Item item = next_item();
            if (item.state() >= 0) {
                item.set_state(static_cast<int16_t>(item.state() - 1));
                synch_chain(item);
            }
            break;
        }
        case 61: { // SET_STATE
            const Item item = next_item();
            int16_t value = static_cast<int16_t>(var_or_word());
            if (value < 0)
                value = 0;
            if (value > 30000)
                value = 30000;
            item.set_state(value);
            synch_chain(item);
            break;
        }
        case 136: { // STATE_TO_VAR
            const int16_t state = next_item().state();
            write_next_var_contents(static_cast<uint16_t>(state));
            break;
        }
        case 92: { // CHILD_TO: subject or object gets the item's first child
            const uint16_t child = next_item().child();
            (var_or_byte() == 1 ? subject_ : object_) = child;
            break;
        }
        case 77:
            condition_ = subject_ != NO_ITEM;
            break;
        case 78:
            condition_ = object_ != NO_ITEM;
            break;
        case 115: { // IS_CLASS
            const Item item = next_item();
            const uint8_t bit = static_cast<uint8_t>(var_or_byte());
            condition_ = (item.class_flags() >> bit) & 1;
            break;
        }
        case 116:   // SET_CLASS
        case 117: { // UNSET_CLASS
            const Item item = next_item();
            const uint16_t mask = static_cast<uint16_t>(1U << var_or_byte());
            item.set_class_flags(static_cast<uint16_t>(
                opcode == 116 ? item.class_flags() | mask : item.class_flags() & ~mask));
            break;
        }
        case 31: { // DESTROY: no parent at all
            set_item_parent(next_item_id(), NO_ITEM, true);
            break;
        }
        case 33: { // PLACE
            const uint16_t item = next_item_id();
            set_item_parent(item, next_item_id(), true);
            break;
        }
        case 55: // GOTO: the player moves
            set_item_parent(PLAYER_ITEM, next_item_id(), true);
            break;
        case 139: { // SET_PARENT, the one that does not notify
            const uint16_t item = next_item_id();
            set_item_parent(item, next_item_id(), false);
            break;
        }
        case 165: { // IS_ADJ_NOUN
            const Item item = next_item();
            const int16_t adjective = next_word();
            const int16_t noun = next_word();
            condition_ = item.valid() && item.adjective() == adjective && item.noun() == noun;
            break;
        }
        case 157: { // GET_OBJECT_VALUE
            const Item item = next_item();
            const uint8_t property = static_cast<uint8_t>(var_or_byte());
            const bool present =
                item.is_object() && property < Object::FLAG_BITS && item.object().has(property);
            write_next_var_contents(present ? item.object().value(property) : 0);
            break;
        }
        case 158: { // SET_OBJECT_VALUE
            const Item item = next_item();
            const uint8_t property = static_cast<uint8_t>(var_or_byte());
            const uint16_t value = var_or_word();
            if (item.is_object() && property < Object::FLAG_BITS && item.object().has(property))
                item.object().set_value(property, value);
            break;
        }
        case 121: { // SET_VGA_ITEM
            const uint8_t slot = static_cast<uint8_t>(var_or_byte());
            const uint16_t item = next_item_id();
            if (slot < OBJECT_SLOTS)
                objects_[slot] = item;
            else
                script_fault(Fault::VGA_SLOT_RANGE, slot);
            break;
        }
        case 151: { // STORE_ITEM
            const uint8_t slot = static_cast<uint8_t>(var_or_byte());
            const uint16_t item = next_item_id();
            if (slot < ITEM_STORE)
                item_store_[slot] = item;
            else
                script_fault(Fault::ITEM_STORE_RANGE, slot);
            break;
        }
        case 152: { // GET_ITEM
            const uint8_t slot = static_cast<uint8_t>(var_or_byte());
            const uint16_t item = slot < ITEM_STORE ? item_store_[slot] : NO_ITEM;
            (var_or_byte() == 1 ? subject_ : object_) = item;
            break;
        }

        // --- bits
        case 153:
            set_bit(var_or_byte(), true);
            break;
        case 154:
            set_bit(var_or_byte(), false);
            break;
        case 155:
            condition_ = !bit(var_or_byte());
            break;
        case 156: { // IS_BIT_SET
            uint16_t number = var_or_byte();
            // The cracked release reads bit 63 where the check wants bit 50
            // (script_e2.cpp:474). Reproducing this bug means the protection
            // check fires, which ScummVM deliberately undoes.
            if (current_table_.valid() && current_table_.id() == 2962 && number == 63)
                number = 50;
            condition_ = bit(number);
            break;
        }
        case 166:
            write_bit(bits2_, var_or_byte(), true);
            break;
        case 167:
            write_bit(bits2_, var_or_byte(), false);
            break;
        case 168:
            condition_ = !read_bit(bits2_, var_or_byte());
            break;
        case 169:
            condition_ = read_bit(bits2_, var_or_byte());
            break;

        // --- a text box's words: its name on hover, its description to speak
        case 66: { // SET_SHORT_TEXT
            const uint8_t slot = static_cast<uint8_t>(var_or_byte());
            const uint16_t string = static_cast<uint16_t>(next_word());
            if (slot < TEXT_BOXES)
                short_text_[slot] = string;
            break;
        }
        case 67: { // SET_LONG_TEXT -- the talkie build carries a speech id too
            const uint8_t slot = static_cast<uint8_t>(var_or_byte());
            const uint16_t string = static_cast<uint16_t>(next_word());
            const uint16_t speech = static_cast<uint16_t>(next_word());
            if (slot < TEXT_BOXES) {
                long_text_[slot] = string;
                long_sound_[slot] = speech;
            }
            break;
        }

        case 179: { // SCREEN_TEXT_LONG_TEXT: a text box's description, said
            // (os1_scnTxtLongText, script_s1.cpp:482-497): room descriptions and
            // conversation answers, by the slot SET_LONG_TEXT filled.
            const uint8_t which = static_cast<uint8_t>(var_or_byte());
            const uint8_t colour = static_cast<uint8_t>(var_or_byte());
            const uint8_t slot = static_cast<uint8_t>(var_or_byte());
            if (slot < TEXT_BOXES) {
                script_text_msg(which, colour, long_text_[slot], long_sound_[slot]);
                voice_cue(long_sound_[slot]);
            }
            break;
        }

        case 187: // FADE_TO_BLACK (os1_specialFade, script_s1.cpp:561-575)
            script_fade_to_black();
            break;
        case 163: // PLAY_EFFECT (os1_playEffect, script_s1.cpp:382-390)
            script_effect(static_cast<uint16_t>(var_or_word()));
            break;
        case 185: // the sound set PLAY_EFFECT indexes, and the speech beside it
            // (os1_loadStrings, script_s1.cpp:544-554)
            script_sound_set(static_cast<uint16_t>(var_or_word()));
            break;

        case 177: { // SCREEN_TEXT_POBJ: an object's description, said
            // (os1_screenTextPObj, script_s1.cpp:392-431): what Look says of a thing
            // that can be picked up. The voice is the object's kOFVoice value, or
            // kOFNumber's plus 3550 (intern.h:235-246).
            constexpr uint8_t TEXT_BIT = 0, NUMBER_BIT = 8, VOICE_BIT = 9;
            constexpr uint16_t NUMBER_VOICES = 3550;
            const uint8_t which = static_cast<uint8_t>(var_or_byte());
            const uint8_t colour = static_cast<uint8_t>(var_or_byte());
            const Item item = next_item();
            if (item.valid() && item.is_object()) {
                const Object object = item.object();
                const uint16_t speech = object.has(VOICE_BIT) ? object.value(VOICE_BIT)
                    : object.has(NUMBER_BIT)
                    ? static_cast<uint16_t>(object.value(NUMBER_BIT) + NUMBER_VOICES)
                    : 0;
                script_text_msg(which, colour, object.value(TEXT_BIT), speech);
                voice_cue(speech);
            }
            break;
        }

        case 65: { // ADD_TEXT_BOX: room scenery and a conversation's choices
            const uint16_t id = static_cast<uint16_t>(var_or_word());
            const uint16_t x = static_cast<uint16_t>(var_or_word());
            const uint16_t y = static_cast<uint16_t>(var_or_word());
            const uint16_t w = static_cast<uint16_t>(var_or_word());
            const uint16_t h = static_cast<uint16_t>(var_or_word());
            const uint8_t which = static_cast<uint8_t>(var_or_byte());
            // The slot it answers with rides in the flags and the verb is 208, which
            // is how the engine marks a text box; its item is the dummy that stands
            // for the player, never null, or a click takes the verb boxes' path
            // (oww_addTextBox, script_ww.cpp:297-307; runit2 a:1b132).
            if (which < TEXT_BOXES)
                script_add_box(id, x, y, w, h, TEXT_CHOICE | which, TEXT_VERB, PLAYER_ITEM);
            break;
        }

        case 96: { // PICTURE: a room's own image script, in a window
            const uint16_t image = static_cast<uint16_t>(var_or_word());
            const uint8_t window = static_cast<uint8_t>(var_or_byte());
            // A zone is the id over a hundred, which is how every id in both VMs is
            // built (setImage, gfx.cpp:1225).
            script_picture(static_cast<uint8_t>(image / 100), image, window);
            break;
        }
        case 97: // LOAD_ZONE
            script_load_zone(static_cast<uint8_t>(var_or_word()));
            break;
        case 98: { // ANIMATE
            const uint16_t sprite = static_cast<uint16_t>(var_or_word());
            const uint8_t window = static_cast<uint8_t>(var_or_byte());
            const int16_t x = static_cast<int16_t>(var_or_word());
            const int16_t y = static_cast<int16_t>(var_or_word());
            const uint8_t palette = static_cast<uint8_t>(var_or_word() & 15);
            // A sprite of 400 and up forgets the sync already sent (script_s1.cpp:296):
            // this one is being started now, so nothing it syncs can have gone past.
            if (sprite >= ANIMATE_FORGETS_SYNC)
                last_sync_ = 0;
            script_animate(window, static_cast<uint8_t>(sprite / 100), sprite, x, y, palette);
            break;
        }
        case 100: // KILL_ANIMATE
            script_kill_animate();
            break;
        case 99: // STOP_ANIMATE: one sprite (oe1_stopAnimate, script_e1.cpp:743)
            script_stop_animate(static_cast<uint16_t>(var_or_word()));
            break;

        case 119: { // WAIT_SYNC, unless a line with no voice let it by (o_waitSync)
            const uint16_t ident = static_cast<uint16_t>(var_or_word());
            if (ident != SPEECH_SYNC || !skip_line_wait_)
                script_wait_sync(ident);
            skip_line_wait_ = false;
            break;
        }
        case 120: // SYNC
            script_sync(static_cast<uint16_t>(var_or_word()));
            break;

        case 127: { // PLAY_TUNE
            const uint16_t music = static_cast<uint16_t>(var_or_word());
            const uint16_t track = static_cast<uint16_t>(var_or_word());
            script_play_tune(music, track);
            break;
        }

        // The engine frees and locks zone memory here, which is a thing this
        // interpreter does not have: zones live in Attic slots and are evicted by
        // use, not by a script's leave. Read and dropped rather than counted as
        // missing, which would say something is owed that is not.
        case 138: // FREEZE_ZONES
        case 175: // LOCK_ZONES
        case 176: // UNLOCK_ZONES
        case 186: // UNFREEZE_ZONES (unfreezeBottom, zones.cpp:37-41)
            break;

        case 107: { // ADD_BOX
            // The id carries the flags above it, a thousand to the bit
            // (script.cpp:661), and an x past a thousand means the verb is one the
            // engine marks (:684).
            uint16_t id = static_cast<uint16_t>(var_or_word());
            const uint8_t flags = static_cast<uint8_t>(id / 1000);
            id = static_cast<uint16_t>(id % 1000);
            uint16_t x = static_cast<uint16_t>(var_or_word());
            const uint16_t y = static_cast<uint16_t>(var_or_word());
            const uint16_t w = static_cast<uint16_t>(var_or_word());
            const uint16_t h = static_cast<uint16_t>(var_or_word());
            const uint16_t item = next_item_id();
            uint16_t verb = static_cast<uint16_t>(var_or_word());
            if (x >= 1000) {
                verb = static_cast<uint16_t>(verb + 0x4000);
                x = static_cast<uint16_t>(x - 1000);
            }
            script_add_box(id, x, y, w, h, flags, verb, item);
            break;
        }

        case DEL_BOX:
        case ENABLE_BOX:
        case DISABLE_BOX:
        case IS_BOX: { // defined and not disabled (isBoxDead, verb.cpp:502)
            const bool alive = script_box(static_cast<uint16_t>(var_or_word()), opcode);
            if (opcode == IS_BOX)
                condition_ = alive;
            break;
        }

        case 132: // SAVE_USER_GAME
            script_save_game();
            break;
        case 133: // LOAD_USER_GAME
            script_load_game();
            break;

        case 178: { // GETPATHPOSN: where Walk to should head (os1_getPathPosn)
            const uint16_t x = static_cast<uint16_t>(var_or_word());
            const uint16_t y = static_cast<uint16_t>(var_or_word());
            const uint8_t route_var = static_cast<uint8_t>(var_or_byte());
            const uint8_t point_var = static_cast<uint8_t>(var_or_byte());
            const uint16_t found = script_route_point(x, y);
            set_variable(route_var, static_cast<int16_t>(found >> 8));
            set_variable(point_var, static_cast<int16_t>(found & 0xFF));
            break;
        }

        case 180: // MOUSE_ON
            script_mouse_on();
            break;

        case 181: // MOUSE_OFF
            script_mouse_off();
            break;

        case 76: { // ADD_TIMEOUT: run a subroutine in so many seconds
            const uint16_t seconds = static_cast<uint16_t>(var_or_word());
            const uint16_t subroutine = static_cast<uint16_t>(var_or_word());
            add_timeout(seconds, subroutine);
            break;
        }
        case 140: { // CLEAR_TIMERS, then the one Simon 1 always wants back
                    // (o_clearTimers, script.cpp:872-878)
            constexpr uint16_t RESTART_SECONDS = 3, RESTART_SUBROUTINE = 160;
            for (uint16_t& subroutine : waiting_)
                subroutine = 0;
            add_timeout(RESTART_SECONDS, RESTART_SUBROUTINE);
            break;
        }
        case 184: // UNLOAD_ZONE: zones here are evicted by use, not by asking
                  // (os1_unloadZone, script_s1.cpp:534)
            (void)var_or_word();
            break;
        case 88: // HALT_ANIMATION
        case 89: // RESTART_ANIMATION
            script_halt_animation(opcode == 88);
            break;
        case 182: // LOAD_BEARD
        case 183: // UNLOAD_BEARD
            script_beard(opcode == 182);
            break;
        case 135: // PAUSE_GAME is the quit prompt (os1_pauseGame,
                  // script_s1.cpp:304); there is nothing to quit to
            break;

        case 63: // SHOW_STRING_NL: a line into the window in force
            script_show_string(static_cast<uint16_t>(next_word()));
            break;
        case 70: { // PRINT_LONG_TEXT: the same, from a slot SET_LONG_TEXT filled,
                   // and silent (oww_printLongText, script_ww.cpp:335-339)
            const uint8_t slot = static_cast<uint8_t>(var_or_byte());
            if (slot < TEXT_BOXES)
                script_show_string(long_text_[slot]);
            break;
        }
        case 101: { // DEFINE_WINDOW
            const uint8_t which = static_cast<uint8_t>(var_or_byte());
            const uint8_t x = static_cast<uint8_t>(var_or_word());
            const uint16_t y = static_cast<uint16_t>(var_or_word());
            const uint8_t cells = static_cast<uint8_t>(var_or_word());
            const uint8_t rows = static_cast<uint8_t>(var_or_word());
            const uint8_t flags = static_cast<uint8_t>(var_or_word());
            (void)var_or_word(); // fill colour: nought in every window below the picture
            script_window(which, x, y, cells, rows, flags);
            break;
        }
        case 102: // CHANGE_WINDOW
            script_use_window(static_cast<uint8_t>(var_or_byte()));
            break;
        case 103: // CLS
            script_clear_window();
            break;
        case 164: { // getDollar2: the command's second item, the player's pick
            // (oe2_getDollar2, script_e2.cpp:587; runit2 FUN_00011788)
            const uint16_t picked = script_pick_object();
            const Item item = db_->item(picked);
            object_ = item.valid() ? picked : NO_ITEM;
            script_noun2_ = item.valid() ? item.noun() : int16_t{-1};
            break;
        }
        case 114: { // DO_ICONS
            const uint16_t item = next_item_id();
            script_do_icons(item, static_cast<uint8_t>(var_or_byte()));
            break;
        }
        case 160: // SET_INK
            script_ink(static_cast<uint8_t>(var_or_byte()));
            break;

        case 161: { // SCREEN_TEXT_BOX: where the next lines are said
            const uint8_t which = static_cast<uint8_t>(var_or_byte());
            const int16_t x = static_cast<int16_t>(var_or_word());
            const uint8_t y = static_cast<uint8_t>(var_or_byte());
            const uint16_t width = static_cast<uint16_t>(var_or_word());
            script_text_box(which, x, y, width);
            break;
        }
        case 162: { // SCREEN_TEXT_MSG: an actor says a line
            const uint8_t which = static_cast<uint8_t>(var_or_byte());
            const uint8_t colour = static_cast<uint8_t>(var_or_byte());
            const uint16_t string = static_cast<uint16_t>(next_word());
            // The talkie build carries the voice's number after the string, as
            // SET_LONG_TEXT does (string.cpp:339-345). Read whether or not anything
            // plays it: the operand is there either way.
            const uint16_t speech = static_cast<uint16_t>(next_word());
            script_text_msg(which, colour, string, speech);
            voice_cue(speech);
            break;
        }

        // --- control flow
        case 68: // END: the engine quits here, operand and all unread
            quit_ = true;
            break;
        case 69: // DONE
            return_ = SCRIPT_DONE;
            break;
        case 83:             // RESCAN
            script_rescan(); // the walk's delay(0), taken as the line ends
            return_ = SCRIPT_RESCAN;
            break;
        case 71: // START_SUB
            start(var_or_word());
            break;
        case 143: { // START_ITEM_SUB: the subroutine on an item's room child
            const Item item = next_item();
            if (item.is_room())
                start(item.room().subroutine_id());
            break;
        }
        case 87: // COMMENT is erased at load and cannot reach memory
            script_fault(Fault::COMMENT_SURVIVED, opcode);
            break;

        default:
            // The operands still have to go. An opcode that leaves them in the stream
            // desynchronises everything after it, so a missing tier would look like a
            // corrupt script instead of a missing tier.
            code_ = skip_operands(code_, opcode);
            ++unimplemented_;
            script_unimplemented(opcode);
            break;
    }
}

} // namespace agos
