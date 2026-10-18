// KeyCodes.h -- engine-level keyboard identifiers.
//
// Values are GLFW's GLFW_KEY_* constants verbatim, so a backend passes its key
// through with no translation table.
//
// Two families are in play, and the numeric values reveal which is which:
//
//   32..96    Printable keys. These are ASCII/UTF-32 code points, which is why
//             WL_KEY_A is 65 and WL_KEY_0 is 48. They line up with a US-layout
//             keyboard, so the identifier can be derived from the character
//             without a lookup -- but that coincidence does NOT hold on other
//             layouts, where the same physical key reports a different code.
//
//   256..348  Non-printable keys: navigation, editing, function keys, keypad,
//             and modifiers. GLFW assigns these from an arbitrary base of 256
//             because they have no character to key off.
//
// Macros rather than an enum so they work in preprocessor branches and case
// labels, and interoperate with the plain ints Input::IsKeyPressed and
// KeyEvent::GetKeyCode carry. The cost is no type safety: any int compiles.

#pragma once

// From glfw3.h

// Sentinel for "no key". Negative, so it cannot collide with a real GLFW key
// code, and distinct from a literal 0 -- note there is no WL_KEY_0 sentinel,
// because WL_KEY_0 is the digit-zero key at 48.
#define WL_KEY_UNKNOWN -1

// Printable keys: US-layout code points. Space first, then the punctuation
// block, digits, then A-Z.
#define WL_KEY_SPACE 32
#define WL_KEY_APOSTROPHE 39 /* ' */
#define WL_KEY_COMMA 44      /* , */
#define WL_KEY_MINUS 45      /* - */
#define WL_KEY_PERIOD 46     /* . */
#define WL_KEY_SLASH 47      /* / */

// Digit row. The trailing comment gives the character each key produces,
// which is why 48 maps to '0'.
#define WL_KEY_0 48
#define WL_KEY_1 49
#define WL_KEY_2 50
#define WL_KEY_3 51
#define WL_KEY_4 52
#define WL_KEY_5 53
#define WL_KEY_6 54
#define WL_KEY_7 55
#define WL_KEY_8 56
#define WL_KEY_9 57

// Remaining US-layout punctuation. Gaps in the numbering are unused code
// points (58 is ':', 60 is '<', 62-64 are '=', '>', '?' and '@' -- GLFW does
// not name the shifted variants of these keys).
#define WL_KEY_SEMICOLON 59 /* ; */
#define WL_KEY_EQUAL 61     /* = */

// Letter keys, consecutive from 65 ('A') through 90 ('Z').
#define WL_KEY_A 65
#define WL_KEY_B 66
#define WL_KEY_C 67
#define WL_KEY_D 68
#define WL_KEY_E 69
#define WL_KEY_F 70
#define WL_KEY_G 71
#define WL_KEY_H 72
#define WL_KEY_I 73
#define WL_KEY_J 74
#define WL_KEY_K 75
#define WL_KEY_L 76
#define WL_KEY_M 77
#define WL_KEY_N 78
#define WL_KEY_O 79
#define WL_KEY_P 80
#define WL_KEY_Q 81
#define WL_KEY_R 82
#define WL_KEY_S 83
#define WL_KEY_T 84
#define WL_KEY_U 85
#define WL_KEY_V 86
#define WL_KEY_W 87
#define WL_KEY_X 88
#define WL_KEY_Y 89
#define WL_KEY_Z 90

// Brackets and the grave accent, closing out the printable range.
#define WL_KEY_LEFT_BRACKET 91  /* [ */
#define WL_KEY_BACKSLASH 92     /* \ */
#define WL_KEY_RIGHT_BRACKET 93 /* ] */
#define WL_KEY_GRAVE_ACCENT 96  /* ` */

// International keys outside the US layout. These have no US-layout meaning
// and appear only on certain regional keyboards; code that tests for them must
// tolerate their absence.
#define WL_KEY_WORLD_1 161      /* non-US #1 */
#define WL_KEY_WORLD_2 162      /* non-US #2 */

// Everything below is GLFW's non-printable range, numbered upward from 256.

// Editing and navigation block, 256-269.
#define WL_KEY_ESCAPE 256
#define WL_KEY_ENTER 257
#define WL_KEY_TAB 258
#define WL_KEY_BACKSPACE 259
#define WL_KEY_INSERT 260
#define WL_KEY_DELETE 261
// Arrow keys. Note the ordering is RIGHT, LEFT, DOWN, UP -- not clockwise.
#define WL_KEY_RIGHT 262
#define WL_KEY_LEFT 263
#define WL_KEY_DOWN 264
#define WL_KEY_UP 265
#define WL_KEY_PAGE_UP 266
#define WL_KEY_PAGE_DOWN 267
#define WL_KEY_HOME 268
#define WL_KEY_END 269

// Lock keys, 280-284. These are toggle states rather than momentary presses,
// so a backend reports them as latched rather than as an edge.
#define WL_KEY_CAPS_LOCK 280
#define WL_KEY_SCROLL_LOCK 281
#define WL_KEY_NUM_LOCK 282
#define WL_KEY_PRINT_SCREEN 283
#define WL_KEY_PAUSE 284

// Function keys F1-F25, 290-314. Modern keyboards stop well before F25; the
// tail exists for completeness with GLFW's enumeration.
#define WL_KEY_F1 290
#define WL_KEY_F2 291
#define WL_KEY_F3 292
#define WL_KEY_F4 293
#define WL_KEY_F5 294
#define WL_KEY_F6 295
#define WL_KEY_F7 296
#define WL_KEY_F8 297
#define WL_KEY_F9 298
#define WL_KEY_F10 299
#define WL_KEY_F11 300
#define WL_KEY_F12 301
#define WL_KEY_F13 302
#define WL_KEY_F14 303
#define WL_KEY_F15 304
#define WL_KEY_F16 305
#define WL_KEY_F17 306
#define WL_KEY_F18 307
#define WL_KEY_F19 308
#define WL_KEY_F20 309
#define WL_KEY_F21 310
#define WL_KEY_F22 311
#define WL_KEY_F23 312
#define WL_KEY_F24 313
#define WL_KEY_F25 314

// Numeric keypad, 320-336.
//
// KP_ prefix means "keypad", distinguishing these from the digit-row keys
// above. Treat them as separate bindings: with numlock off the keypad drives
// navigation and these do not fire.
#define WL_KEY_KP_0 320
#define WL_KEY_KP_1 321
#define WL_KEY_KP_2 322
#define WL_KEY_KP_3 323
#define WL_KEY_KP_4 324
#define WL_KEY_KP_5 325
#define WL_KEY_KP_6 326
#define WL_KEY_KP_7 327
#define WL_KEY_KP_8 328
#define WL_KEY_KP_9 329
// Keypad operators and the dedicated keypad enter/equals, which are distinct
// keys from the digit-row equivalents and from the main-block enter.
#define WL_KEY_KP_DECIMAL 330
#define WL_KEY_KP_DIVIDE 331
#define WL_KEY_KP_MULTIPLY 332
#define WL_KEY_KP_SUBTRACT 333
#define WL_KEY_KP_ADD 334
#define WL_KEY_KP_ENTER 335
#define WL_KEY_KP_EQUAL 336

// Modifiers, 340-348.
//
// Left and right variants are separate identifiers, which is what makes
// distinguishing WASD from arrow keys possible -- a check that cannot be done
// with a single shift/control code.
#define WL_KEY_LEFT_SHIFT 340
#define WL_KEY_LEFT_CONTROL 341
#define WL_KEY_LEFT_ALT 342
#define WL_KEY_LEFT_SUPER 343
#define WL_KEY_RIGHT_SHIFT 344
#define WL_KEY_RIGHT_CONTROL 345
#define WL_KEY_RIGHT_ALT 346
#define WL_KEY_RIGHT_SUPER 347
#define WL_KEY_MENU 348

// Highest valid key code. For looping over the key range. Note this is
// WL_KEY_MENU (348), not a printable key -- the printable keys top out at 162,
// so a loop bounded by LAST spans the whole non-printable range too.
#define WL_KEY_LAST WL_KEY_MENU