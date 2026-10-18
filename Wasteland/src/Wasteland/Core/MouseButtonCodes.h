// MouseButtonCodes.h -- engine-level mouse button identifiers.
//
// Values are taken verbatim from GLFW's MOUSE_BUTTON_* constants (see the
// attribution above the block), so a GLFW code needs no translation and a
// backend can pass its own value straight through.
//
// These are macros rather than an enum so they are usable in preprocessor
// conditionals and case labels, and so they interoperate with the plain ints
// that Input::IsMouseButtonPressed and MouseButtonEvent::GetMouseButton
// carry. The trade-off is no type safety: any int will do, and a mistyped
// constant compiles silently.

#pragma once

// From glfw3.h
//
// GLFW numbers buttons 1..8 left to right. The engine mirrors that numbering
// exactly rather than renumbering from zero-of-meaning, so button 1 is the
// leftmost physical button.

// Mouse button identifiers, matching GLFW's MOUSE_BUTTON_* values.
//
// 1 through 8 are physical positions. On a standard three-button mouse only
// 1, 2 and 3 are meaningful; 4 through 8 exist for extra buttons on mice that
// have them, and are never reported by hardware that lacks them.
#define WL_MOUSE_BUTTON_1         0
#define WL_MOUSE_BUTTON_2         1
#define WL_MOUSE_BUTTON_3         2
#define WL_MOUSE_BUTTON_4         3
#define WL_MOUSE_BUTTON_5         4
#define WL_MOUSE_BUTTON_6         5
#define WL_MOUSE_BUTTON_7         6
#define WL_MOUSE_BUTTON_8         7

// Highest valid button index, aliased to button 8.
//
// For looping over the button range: `for (int b = WL_MOUSE_BUTTON_1;
// b <= WL_MOUSE_BUTTON_LAST; ++b)`.
#define WL_MOUSE_BUTTON_LAST      WL_MOUSE_BUTTON_8

// Named aliases for the three buttons a standard mouse has. Prefer these over
// the numbered forms: they say which physical button is meant and survive a
// change to the numbering scheme.
#define WL_MOUSE_BUTTON_LEFT      WL_MOUSE_BUTTON_1
#define WL_MOUSE_BUTTON_RIGHT     WL_MOUSE_BUTTON_2
#define WL_MOUSE_BUTTON_MIDDLE    WL_MOUSE_BUTTON_3