/*
  PreviousTurbo - gui_internal.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  What the parts of the window's GUI share with each other.
*/

#ifndef PREV_GUI_INTERNAL_H
#define PREV_GUI_INTERNAL_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "menu_model.h"

namespace ptgui {

#if defined(__APPLE__)
constexpr const char* kAlt = "Option";
#else
constexpr const char* kAlt = "Alt";
#endif
constexpr const char* kAppName = "PreviousTurbo";

struct State {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    ImGuiContext* ctx = nullptr;
    float scale = 1.0f;     // the desktop's UI scale
    float density = 1.0f;   // pixels per window coordinate
    bool native_menus = false;
    int pending_frames = 3;  // frames to draw even if nothing seems to change
    bool repaint = true;     // the emulated screen changed
    uint64_t next_frame_ns = 0;
    uint64_t next_status_ns = 0;
    int modal_depth = 0;     // loops running for open dialogs
    SDL_FRect dest{};        // where the emulated screen is, in window coordinates
    float dest_scale = 1.0f; // window coordinates per screen pixel
    uint32_t buttons_down = 0;            // buttons whose press went to the machine
    std::vector<bool> keys_down = std::vector<bool>(SDL_SCANCODE_COUNT, false);
    std::vector<std::function<void()>> posted;
    // Sees events before anything else while set (defining a shortcut key);
    // true: taken.
    std::function<bool(const SDL_Event&)> event_hook;
    bool about_open = false, keys_open = false;
};

State& st();

// Run after the current frame, outside ImGui (menu actions, dialogs that
// run their own loop).
void post(std::function<void()> action);

// Keeps the window alive (events, frames) while `still_open` is true; the
// emulated machine gets no input meanwhile.
void run_modal(const std::function<bool()>& still_open);

// Asks for a frame now and for the next `n`.
void wake(int n = 2);

// --- Menus (gui.cpp) ---
std::vector<MenuEntry> menus();
// The keys of a shortcut action as shown in menus ("Ctrl+Alt+F", "F12"), or "".
std::string shortcut_keys(int shortcut);

// --- Status bar (statusbar.cpp) ---
void draw_status_bar();
// True if what the status bar shows changed since the last call.
bool status_changed(uint64_t now_ns);

// --- Dialogs (dialogs.cpp) ---
void draw_dialogs();
bool dialog_open();

// --- Settings (settings.cpp) ---
void draw_settings();
bool settings_open();

// --- File choosers (files.cpp) ---
enum class Pick { Open, Save, Folder };
struct FileFilter {
    const char* name;
    const char* pattern;  // "img;iso" or "*"
};
// Shows the system's file chooser; `done` gets the path (empty if cancelled)
// from the main loop.
void choose(Pick kind, const std::string& start, std::vector<FileFilter> filters,
            std::function<void(const std::string&)> done);
// The same, waiting for the answer.
std::string choose_now(Pick kind, const std::string& start, std::vector<FileFilter> filters);
// Applies answers that arrived (called each frame).
void deliver_choices();

// --- Helpers ---
std::string file_name(const std::string& path);
std::string folder_of(const std::string& path);
void help_marker(const char* text);

}  // namespace ptgui

#endif /* PREV_GUI_INTERNAL_H */
