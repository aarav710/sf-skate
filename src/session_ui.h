// What the "PLAY WITH FRIENDS" panel needs besides drawing: the settings file (~/.sfskate), text fields, keyboard
// and mouse input that knows when a text field has the keyboard (so typing a name doesn't press game keys), and a
// small input script for tests (SFSKATE_UI_SCRIPT) that types, clicks and pastes the way a player would.
#pragma once
#include "raylib.h"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sui {

// ---------------------------------------------------------------- ~/.sfskate
// key=value lines. Keys this version doesn't know are kept as they were, so an older game doesn't drop a newer one's.
struct Settings {
    std::string name, relay;                                   // "" = not set
    std::vector<std::pair<std::string, std::string>> other;
};
std::string settingsPath();                                    // $HOME/.sfskate ("" with no HOME)
// Missing, unreadable or garbled files give what could be read (never fails): a bad name or relay line is skipped.
Settings loadSettings(const std::string& path);
bool saveSettings(const std::string& path, const Settings& s);  // false if it couldn't be written (the old file stays)
std::string cleanRelay(const std::string& in);                 // printable ASCII, no spaces, at most 128 bytes

// ---------------------------------------------------------------- text fields
struct TextField {
    enum Kind { NAME, CODE, ADDRESS };
    Kind kind;
    std::string text;
    bool edited = false;    // the player changed it (since it was filled in)
    bool fresh = false;     // just got the keyboard: what's typed (or pasted) first replaces the text, as if selected
    explicit TextField(Kind k) : kind(k) {}
    // one typed character (a Unicode code point): kept if this field takes it and it fits
    void type(uint32_t cp);
    void backspace();       // the last character (whole UTF-8 sequence), or all of it if fresh
    void paste(const std::string& clip);
    size_t maxBytes() const;
};
// What pasting into the join box keeps of the clipboard: its letters and digits, upper-cased, without spaces and
// dashes; if that is longer than a code, the last 6 (so "Code ABC234" or a sfskate://join/ABC234 link work).
std::string codeFromPaste(const std::string& clip);

// ---------------------------------------------------------------- input
// Call once a frame before anything reads input.
void beginFrame();
bool keyPressed(int key);         // the real keyboard or the script
bool keyDown(int key);
bool keyRepeat(int key);          // pressed, or held long enough to repeat
uint32_t charPressed();           // the next typed character, 0 when none
std::string clipboard();
void setClipboard(const std::string& s);
Vector2 mouse();
bool mousePressed();

// The text field that has the keyboard (null: none). While one has it (or had it at any point this frame) game keys
// read as not pressed: gameKeyPressed/gameKeyDown are what the game's hotkeys go through.
void focus(TextField* f);
TextField* focused();
bool typing();
inline bool gameKeyPressed(int key) { return !typing() && keyPressed(key); }
inline bool gameKeyDown(int key) { return !typing() && keyDown(key); }

// Where each named control was drawn, so the script can click it like a player would (center of the box).
void place(const std::string& id, Rectangle r);

// ---------------------------------------------------------------- test script (SFSKATE_UI_SCRIPT)
// Steps separated by ';' or newlines:
//   wait N (frames)  sleep S (seconds)  key NAME (ENTER, TAB, ESCAPE, BACKSPACE, a letter or digit)
//   type TEXT (one character a frame, with its key pressed too, as a keyboard does)  paste (Ctrl+V)
//   clipboard TEXT  click ID  shot FILE (any path)  until COND [SECS] (COND is up to the game; gives up after SECS, default 60)
//   do WHAT (the game does something by name)  quit
// The game answers `until` and `do` through `game` (cond/what, true = met/known).
void startScript(const std::string& script, std::function<bool(const std::string& cmd, const std::string& arg)> game);
bool scriptRunning();
bool scriptQuit();                // a quit step ran (or the script failed)
// test: inject a key press this frame (as the script does)
void injectKey(int key, bool ctrl = false);

}  // namespace sui
