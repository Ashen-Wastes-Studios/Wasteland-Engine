// GamepadButtonCodes.h -- engine-level gamepad button and axis identifiers.
//
// Values mirror GLFW's GAMEPAD_BUTTON_* and GAMEPAD_AXIS_* constants (see the
// attribution above each block), so a GLFW value passes through untranslated.
//
// Macros rather than an enum, for the same reasons as MouseButtonCodes.h:
// usable in preprocessor branches and case labels, and interchangeable with
// the plain ints the Gamepad API carries. No type safety comes with that.
//
// Axes are indices into a fixed-size array, not enum values with meaning -- see
// the axis block below for the important caveat about trigger range.

#pragma once

// From glfw3.h
//
// The face-button names below follow the Xbox layout (A/B/X/Y on the bottom
// and right of the diamond). The CROSS/CIRCLE/SQUARE/TRIANGLE aliases exist for
// the PlayStation layout, where the same physical positions carry those names.
// Using the positional alias rather than the platform name is what lets one
// piece of code drive both controller families.

// Digital buttons, 0 through 14, in GLFW's order.
//
// 0-3 are the face buttons, 4-5 the shoulder bumpers, 6-7 back/start, 8 the
// guide button, 9-10 the thumbstick clicks, and 11-14 the D-pad. Values are
// indices, so they double as offsets into GLFW's button state array.

// Face buttons, named per the Xbox layout.
#define WL_GAMEPAD_BUTTON_A 0
#define WL_GAMEPAD_BUTTON_B 1
#define WL_GAMEPAD_BUTTON_X 2
#define WL_GAMEPAD_BUTTON_Y 3

// Shoulder bumpers (often called triggers on older controllers).
#define WL_GAMEPAD_BUTTON_LEFT_BUMPER 4
#define WL_GAMEPAD_BUTTON_RIGHT_BUMPER 5

// Back/select and start/menu.
#define WL_GAMEPAD_BUTTON_BACK 6
#define WL_GAMEPAD_BUTTON_START 7

// The vendor guide/logo button in the middle of the pad.
#define WL_GAMEPAD_BUTTON_GUIDE 8

// Clicking the thumbsticks in.
#define WL_GAMEPAD_BUTTON_LEFT_THUMB 9
#define WL_GAMEPAD_BUTTON_RIGHT_THUMB 10

// D-pad, clockwise from up.
#define WL_GAMEPAD_BUTTON_DPAD_UP 11
#define WL_GAMEPAD_BUTTON_DPAD_RIGHT 12
#define WL_GAMEPAD_BUTTON_DPAD_DOWN 13
#define WL_GAMEPAD_BUTTON_DPAD_LEFT 14

// Highest valid button index. For looping over the button range.
#define WL_GAMEPAD_BUTTON_LAST WL_GAMEPAD_BUTTON_DPAD_LEFT

// Face-button aliases named per the PlayStation layout.
//
// These are not different buttons -- CROSS is the same physical button as A.
// They exist so code can be written against whichever naming convention the
// target controller uses without duplicating the logic.
#define WL_GAMEPAD_BUTTON_CROSS WL_GAMEPAD_BUTTON_A
#define WL_GAMEPAD_BUTTON_CIRCLE WL_GAMEPAD_BUTTON_B
#define WL_GAMEPAD_BUTTON_SQUARE WL_GAMEPAD_BUTTON_X
#define WL_GAMEPAD_BUTTON_TRIANGLE WL_GAMEPAD_BUTTON_Y

// Analog axis indices, 0 through 5.
//
// IMPORTANT: every axis, triggers included, is reported in the range
// [-1.0, 1.0] inclusive. The vendored GLFW 3.4 header states this directly:
// "The states of each gamepad axis, in the range -1.0 to 1.0 inclusive."
//
// The sticks rest at 0 and travel in both directions to -1 and +1. The
// triggers rest at -1 (fully released) and travel to +1 (fully pressed), so
// they do NOT share the sticks' centre. Code that assumes a symmetric range
// and treats 0 as "released" will read a resting trigger as half-pressed.
// Gamepad::GetTrigger performs the (-1..1 -> 0..1) conversion; use it rather
// than doing that arithmetic at the call site.

// Left stick, X then Y. Range [-1, 1], resting at 0.
#define WL_GAMEPAD_AXIS_LEFT_X 0
#define WL_GAMEPAD_AXIS_LEFT_Y 1

// Right stick, X then Y. Range [-1, 1], resting at 0.
#define WL_GAMEPAD_AXIS_RIGHT_X 2
#define WL_GAMEPAD_AXIS_RIGHT_Y 3

// Left trigger. Range [-1, 1], resting at -1 (released). See the note above.
#define WL_GAMEPAD_AXIS_LEFT_TRIGGER 4

// Right trigger. Range [-1, 1], resting at -1 (released). See the note above.
#define WL_GAMEPAD_AXIS_RIGHT_TRIGGER 5

// Highest valid axis index. For looping over the axis range.
#define WL_GAMEPAD_AXIS_LAST WL_GAMEPAD_AXIS_RIGHT_TRIGGER