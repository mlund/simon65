// Operand tables for both AGOS bytecode machines, Simon 1 CD32 talkie.

// Generated from ScummVM's opcode tables; do not edit.
// clang-format off
#pragma once

#include <stdint.h>

namespace agos {

/// How readSingleOpcode reads one operand (subroutine.cpp:756). The class
/// covers both forms of the stream: on disk BYTE is one byte, or two when the
/// first is 0xFF; ITEM and TEXT are two, or six when the word is not one of
/// their sentinels; WORD is always two. In memory every operand is two bytes
/// except BYTE, which keeps its 0xFF escape.
enum class DiskArg : uint8_t { END, WORD, BYTE, ITEM, TEXT };

inline constexpr uint8_t SCRIPT_OPCODES = 188;
inline constexpr uint8_t VIDEO_OPCODES = 64;

/// Where an opcode's operands start in SCRIPT_ARG_POOL.
inline constexpr uint8_t SCRIPT_ARG_INDEX[SCRIPT_OPCODES] = {
    7, 76, 76, 76, 76, 76, 76, 82, 82, 82, 82, 21,
    21, 41, 41, 41, 41, 51, 51, 51, 51, 82, 82, 6,
    76, 76, 76, 13, 59, 82, 76, 76, 82, 82, 54, 58,
    51, 21, 75, 59, 21, 21, 41, 41, 41, 51, 51, 41,
    41, 51, 51, 41, 51, 41, 21, 76, 59, 59, 82, 76,
    76, 13, 21, 86, 86, 16, 85, 45, 86, 7, 21, 6,
    62, 76, 76, 76, 5, 7, 7, 88, 82, 76, 21, 7,
    59, 54, 66, 86, 7, 7, 59, 59, 59, 21, 51, 54,
    20, 6, 23, 6, 7, 0, 21, 7, 21, 21, 51, 8,
    6, 6, 6, 4, 29, 34, 59, 59, 59, 59, 6, 6,
    6, 75, 7, 7, 6, 76, 54, 5, 6, 6, 30, 51,
    7, 7, 7, 7, 59, 21, 7, 82, 7, 75, 6, 76,
    59, 59, 59, 59, 59, 59, 59, 75, 51, 21, 21, 21,
    21, 54, 62, 59, 21, 39, 44, 6, 7, 70, 21, 21,
    21, 21, 86, 86, 21, 7, 76, 7, 7, 74, 49, 78,
    7, 7, 7, 7, 6, 6, 7, 7,
};

/// END-terminated operand runs, sharing tails: 34 distinct
/// patterns in 91 entries rather than 133.
inline constexpr DiskArg SCRIPT_ARG_POOL[91] = {
    DiskArg::BYTE, DiskArg::WORD, DiskArg::WORD, DiskArg::WORD, DiskArg::WORD,
    DiskArg::WORD, DiskArg::WORD, DiskArg::END, DiskArg::WORD, DiskArg::WORD,
    DiskArg::WORD, DiskArg::WORD, DiskArg::WORD, DiskArg::ITEM, DiskArg::WORD,
    DiskArg::END, DiskArg::WORD, DiskArg::WORD, DiskArg::WORD, DiskArg::WORD,
    DiskArg::WORD, DiskArg::BYTE, DiskArg::END, DiskArg::WORD, DiskArg::BYTE,
    DiskArg::WORD, DiskArg::WORD, DiskArg::WORD, DiskArg::END, DiskArg::WORD,
    DiskArg::BYTE, DiskArg::WORD, DiskArg::WORD, DiskArg::END, DiskArg::ITEM,
    DiskArg::BYTE, DiskArg::WORD, DiskArg::WORD, DiskArg::END, DiskArg::BYTE,
    DiskArg::WORD, DiskArg::BYTE, DiskArg::WORD, DiskArg::END, DiskArg::BYTE,
    DiskArg::BYTE, DiskArg::TEXT, DiskArg::WORD, DiskArg::END, DiskArg::WORD,
    DiskArg::WORD, DiskArg::BYTE, DiskArg::BYTE, DiskArg::END, DiskArg::ITEM,
    DiskArg::BYTE, DiskArg::BYTE, DiskArg::END, DiskArg::BYTE, DiskArg::ITEM,
    DiskArg::BYTE, DiskArg::END, DiskArg::ITEM, DiskArg::BYTE, DiskArg::WORD,
    DiskArg::END, DiskArg::ITEM, DiskArg::ITEM, DiskArg::BYTE, DiskArg::END,
    DiskArg::ITEM, DiskArg::WORD, DiskArg::WORD, DiskArg::END, DiskArg::BYTE,
    DiskArg::BYTE, DiskArg::ITEM, DiskArg::END, DiskArg::BYTE, DiskArg::BYTE,
    DiskArg::BYTE, DiskArg::END, DiskArg::ITEM, DiskArg::ITEM, DiskArg::END,
    DiskArg::BYTE, DiskArg::TEXT, DiskArg::END, DiskArg::ITEM, DiskArg::TEXT,
    DiskArg::END,
};

/// Operand bytes per video opcode (opcodeParamLenSimon1, vga.cpp:328), as
/// vcSkipNextInstruction uses them.
inline constexpr uint8_t VIDEO_PARAM_LEN[VIDEO_OPCODES] = {
    0, 6, 2, 10, 6, 4, 2, 2,
    4, 4, 10, 0, 2, 2, 2, 2,
    2, 0, 2, 0, 4, 2, 4, 2,
    8, 0, 10, 0, 8, 0, 2, 2,
    4, 0, 0, 4, 4, 2, 2, 4,
    4, 4, 4, 2, 2, 2, 2, 4,
    0, 2, 2, 2, 2, 4, 6, 6,
    0, 0, 0, 0, 2, 6, 0, 0,
};

/// Bit n set where Simon 1 installs video opcode n (vga.cpp:39,
/// vga_s1.cpp:31); the rest are an error() if ever reached.
inline constexpr uint64_t VIDEO_INSTALLED = 0xF89FFFFFEFF7FFFEULL;

/// The one entry above that lies: setPathfinderItem takes a word and then a
/// 999-terminated (x,y) list (vga_s1.cpp:56-58), which the table calls 0.
inline constexpr uint8_t VIDEO_PATHFIND_OPCODE = 17;

// Mnemonics are for host diagnostics; the 6502 never needs a name.
#ifndef __mos__
/// nullptr where Simon 1 implements no such opcode.
inline constexpr const char *SCRIPT_NAME[SCRIPT_OPCODES] = {
    "NOT", "AT", "NOT_AT", nullptr, nullptr, "CARRIED", "NOT_CARRIED", "IS_AT",
    nullptr, nullptr, nullptr, "IS_ZERO", "ISNOT_ZERO", "IS_EQ", "IS_NEQ",
    "IS_LE", "IS_GE", "IS_EQF", "IS_NEQF", "IS_LEF", "IS_GEF", nullptr,
    nullptr, "CHANCE", nullptr, "IS_ROOM", "IS_OBJECT", "ITEM_STATE_IS",
    "OBJECT_HAS_FLAG", nullptr, nullptr, "SET_NO_PARENT", nullptr,
    "SET_PARENT", nullptr, nullptr, "MOVE", nullptr, nullptr, nullptr, nullptr,
    "ZERO", "SET", "ADD", "SUB", "ADDF", "SUBF", "MUL", "DIV", "MULF", "DIVF",
    "MOD", "MODF", "RANDOM", nullptr, "SET_A_PARENT", "SET_CHILD2_BIT",
    "CLEAR_CHILD2_BIT", "MAKE_SIBLING", "INC_STATE", "DEC_STATE", "SET_STATE",
    "SHOW_INT", "SHOW_STRING_NL", "SHOW_STRING", "ADD_TEXT_BOX",
    "SET_SHORT_TEXT", "SET_LONG_TEXT", "END", "DONE", "SHOW_STRING_AR3",
    "START_SUB", nullptr, nullptr, nullptr, nullptr, "ADD_TIMEOUT",
    "IS_SUBJECT_ITEM_EMPTY", "IS_OBJECT_ITEM_EMPTY", "CHILD_FR2_IS",
    "IS_ITEM_EQ", nullptr, "DEBUG", "RESCAN", nullptr, nullptr, nullptr,
    "COMMENT", "STOP_ANIMATION", "RESTART_ANIMATION", "GET_PARENT", "GET_NEXT",
    "GET_CHILDREN", nullptr, nullptr, nullptr, "PICTURE", "LOAD_ZONE",
    "ANIMATE", "STOP_ANIMATE", "KILL_ANIMATE", "DEFINE_WINDOW",
    "CHANGE_WINDOW", "CLS", "CLOSE_WINDOW", nullptr, nullptr, "ADD_BOX",
    "DEL_BOX", "ENABLE_BOX", "DISABLE_BOX", "MOVE_BOX", nullptr, nullptr,
    "DO_ICONS", "IS_CLASS", "SET_CLASS", "UNSET_CLASS", nullptr, "WAIT_SYNC",
    "SYNC", "DEF_OBJ", nullptr, nullptr, nullptr, "IS_SIBLING_WITH_A",
    "DO_CLASS_ICONS", "PLAY_TUNE", nullptr, nullptr, "SET_ADJ_NOUN", nullptr,
    "SAVE_USER_GAME", "LOAD_USER_GAME", "STOP_TUNE", "PAUSE", "COPY_SF",
    "RESTORE_ICONS", "FREEZE_ZONES", "SET_PARENT_SPECIAL", "CLEAR_TIMERS",
    "SET_M1_OR_M3", "IS_BOX", "START_ITEM_SUB", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, "STORE_ITEM", "GET_ITEM", "SET_BIT",
    "CLEAR_BIT", "IS_BIT_CLEAR", "IS_BIT_SET", "GET_ITEM_PROP",
    "SET_ITEM_PROP", nullptr, "SET_INK", "SETUP_TEXT", "PRINT_STR",
    "PLAY_EFFECT", "getDollar2", "IS_ADJ_NOUN", "SET_BIT2", "CLEAR_BIT2",
    "IS_BIT2_CLEAR", "IS_BIT2_SET", nullptr, nullptr, nullptr, nullptr,
    nullptr, "LOCK_ZONES", "UNLOCK_ZONES", "SCREEN_TEXT_POBJ", "GETPATHPOSN",
    "SCREEN_TEXT_LONG_TEXT", "MOUSE_ON", "MOUSE_OFF", "LOAD_BEARD",
    "UNLOAD_BEARD", "UNLOAD_ZONE", "LOAD_SOUND_FILES", "UNFREEZE_ZONES",
    "FADE_TO_BLACK",
};

inline constexpr const char *VIDEO_NAME[VIDEO_OPCODES] = {
    "RET", "FADEOUT", "CALL", "NEW_SPRITE", "FADEIN", "IF_EQUAL",
    "IF_OBJECT_HERE", "IF_OBJECT_NOT_HERE", "IF_OBJECT_IS_AT",
    "IF_OBJECT_STATE_IS", "DRAW", "CLEAR_PATHFIND_ARRAY", "DELAY",
    "SET_SPRITE_OFFSET_X", "SET_SPRITE_OFFSET_Y", "SYNC", "WAIT_SYNC",
    "SET_PATHFIND_ITEM", "JUMP_REL", "CHAIN_TO", "SET_REPEAT", "END_REPEAT",
    "SET_PALETTE", "SET_PRIORITY", "SET_SPRITE_XY", "HALT_SPRITE",
    "SET_WINDOW", "RESET", "PLAY_SOUND", "STOP_ALL_SOUNDS", "SET_FRAME_RATE",
    "SET_WINDOW", "COPY_VAR", "MOUSE_ON", "MOUSE_OFF", "CLEAR_WINDOW",
    "SET_WINDOW_IMAGE", "SET_SPRITE_OFFSET_Y", "IF_VAR_NOT_ZERO", "SET_VAR",
    "ADD_VAR", "SUB_VAR", "DELAY_IF_NOT_EQ", "IF_BIT_SET", "IF_BIT_CLEAR",
    "SET_SPRITE_X", "SET_SPRITE_Y", "ADD_VAR_F", "COMPUTE_YOFS", "SET_BIT",
    "CLEAR_BIT", "ENABLE_BOX", "PLAY_EFFECT", "DUMMY_53", "DUMMY_54",
    "MOVE_BOX", "DUMMY_56", "BLACK_PALETTE", "DUMMY_58", "IF_SPEECH",
    "STOP_ANIMATE", "MASK", "FASTFADEOUT", "FASTFADEIN",
};

/// Video operand letters (debug.h): b=1 w,d,v,i=2 j,x=0 q=999-terminated list.
inline constexpr const char *VIDEO_ARGS[VIDEO_OPCODES] = {
    "x", "ddd", "w", "ddddd", "ddd", "vdj", "dj", "dj", "ddj", "ddj", "ddddd",
    "", "w", "d", "d", "d", "d", "dq", "i", "", "dd", "i", "dd", "d", "wiid",
    "x", "ddddd", "", "dddd", "", "d", "d", "vv", "", "", "dd", "dd", "v",
    "vj", "vd", "vd", "vd", "vd", "dj", "dj", "v", "v", "vv", "", "d", "d",
    "d", "d", "dd", "ddd", "ddd", "", "", "", "j", "d", "wdd", "", "",
};
#endif

} // namespace agos
