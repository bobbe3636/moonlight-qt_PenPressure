#pragma once

// The stream menu: a small floating Moonlight button on the stream window that opens a menu
// with every in-stream command and its shortcut (Parsec style). Drag the button anywhere in
// the window; it stays where it's dropped and remembers its place. Windows only; elsewhere
// these are no-ops.

class Session;
class StreamMenu;
struct SDL_Window;

StreamMenu* streamMenuCreate(Session* session, SDL_Window* window);
void streamMenuDestroy(StreamMenu* menu);

// Ctrl+Alt+Shift+B: show/hide the button (or open the menu where the button can't be shown)
void streamMenuToggle(StreamMenu* menu);

// Open the menu (its shortcut, e.g. in immersive mode): from the button, else at the cursor
void streamMenuOpen(StreamMenu* menu);

// An extra screen's window: its disconnect / quit shortcuts act on every screen, as the menu's
// commands do (the main window runs them). False in the main window.
bool streamMenuForwardShortcut(StreamMenu* menu, char letter);
