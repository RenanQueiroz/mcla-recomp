// DualSense adaptive triggers on the PC (src/pc_triggers.cpp). The PS5 build has its own
// (ps5/game/ps5_triggers.h) and does not compile this.

#pragma once

// OnPostSetup: starts the worker thread (it sleeps while mcla_adaptive_triggers is off).
void MclaPcTriggersStart();
// Every game update, on the thread that polls the pad (mcla_frame_delta): the pedals' positions
// as the game sees them, read only while the triggers are in use.
void MclaPcTriggersFrame();
// The game is ending (the window's close, QUIT GAME, RESTART GAME, the title ending on its own):
// both triggers are released and the worker stops. Safe to call more than once and from any thread.
void MclaPcTriggersShutdown();

// True while the pause menu's movie is being advanced (src/frame_timing.cpp, the UI movie advance
// hook): the pause menu is up, whether or not the car moves. Not set when mcla_ui_timing_fix is off.
bool MclaPauseMenuUp();
