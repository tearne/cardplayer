// Chess game module — PoC.
//
// A self-contained side activity: own board, own renderer, own keyboard
// handling. The host app delegates draw and key dispatch to this module
// while chess mode is active; everything else (audio, browser state)
// continues untouched in the background.
//
// State persistence rides on the same Preferences/NVS store the rest of
// the app uses, under a separate namespace.

#pragma once

#include <M5GFX.h>
#include <M5Cardputer.h>

namespace chess {

void initAtBoot();          // load persisted state, or set up a new game
void enter();               // become active; redraws
void exit();                // become inactive; saves state
bool active();

// Repaint the chess screen. Caller is responsible for presenting the canvas.
void render(M5Canvas& canvas);

// Handle a keystroke while chess is active.
// Returns true if the key was consumed by chess. If it returns false,
// the caller should leave chess mode (preserving state).
bool handleKey(const Keyboard_Class::KeysState& state);

}  // namespace chess
