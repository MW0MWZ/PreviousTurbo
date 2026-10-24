/*
  PreviousTurbo - gui.cpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The main window: the emulated screen between a menu bar and a status bar,
  with the Settings window and the other dialogs drawn over it.
*/

#include "gui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "gui_internal.h"

extern "C" {
#include "main.h"
#include "configuration.h"
#include "dialog.h"
#include "event.h"
#include "keymap.h"
#include "screen.h"
#include "sdlscreen.h"
#include "shortcut.h"
#include "statusbar.h"
#include "grab.h"
}

namespace ptgui {

namespace {

State g_state;

constexpr uint64_t kFrameNs = 1000000000ull / 60;

// Window coordinates of the emulated screen, as large as fits between the
// bars with its shape kept.
void layout(float top, float bottom) {
    State& s = st();
    int ww = 0, wh = 0;
    SDL_GetWindowSize(s.window, &ww, &wh);
    const float area_w = static_cast<float>(ww), area_h = std::max(1.0f, bottom - top);
    const float k = std::min(area_w / static_cast<float>(screen_w), area_h / static_cast<float>(screen_h));
    s.dest_scale = k > 0 ? k : 1.0f;
    s.dest.w = static_cast<float>(screen_w) * s.dest_scale;
    s.dest.h = static_cast<float>(screen_h) * s.dest_scale;
    s.dest.x = std::floor((area_w - s.dest.w) / 2);
    s.dest.y = top + std::floor((area_h - s.dest.h) / 2);
}

float bar_height() {
    // ImGui's frame height: the 13 pixel font plus twice the frame padding.
    return (13.0f + 6.0f) * st().scale;
}

bool menu_bar_shown() {
    return !st().native_menus && !bInFullScreen;
}

void layout_now() {
    State& s = st();
    int ww = 0, wh = 0;
    SDL_GetWindowSize(s.window, &ww, &wh);
    const float top = menu_bar_shown() ? bar_height() : 0;
    const float bottom = static_cast<float>(wh) - (ConfigureParams.Screen.bShowStatusbar ? bar_height() : 0);
    layout(top, bottom);
}

bool is_input(Uint32 type) {
    switch (type) {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_TEXT_EDITING:
            return true;
        default:
            return false;
    }
}

// A menu item chosen from the macOS menu bar: ignored while a dialog is open.
void post_from_menu(std::function<void()> action) {
    if (st().modal_depth == 0) post(std::move(action));
}

void draw_entries(const std::vector<MenuEntry>& entries) {
    int id = 0;
    for (const MenuEntry& e : entries) {
        ImGui::PushID(id++);
        switch (e.kind) {
            case MenuEntry::Kind::Separator: ImGui::Separator(); break;
            case MenuEntry::Kind::Heading: ImGui::TextDisabled("%s", e.label.c_str()); break;
            case MenuEntry::Kind::Submenu:
                if (ImGui::BeginMenu(e.label.c_str(), e.enabled)) {
                    draw_entries(e.children);
                    ImGui::EndMenu();
                }
                break;
            case MenuEntry::Kind::Item:
                if (ImGui::MenuItem(e.label.c_str(), e.keys.empty() ? nullptr : e.keys.c_str(), e.checked, e.enabled) &&
                    e.action)
                    post(e.action);
                if (!e.tooltip.empty()) ImGui::SetItemTooltip("%s", e.tooltip.c_str());
                break;
        }
        ImGui::PopID();
    }
}

void draw_menu() {
    ImGui::BeginDisabled(st().modal_depth > 0);
    if (ImGui::BeginMainMenuBar()) {
        draw_entries(menus());
        ImGui::EndMainMenuBar();
    }
    ImGui::EndDisabled();
}

// The emulated screen's textures, scaled into the picture area.
void draw_pictures(const ImVec2& fb) {
    State& s = st();
    SDL_Texture* tex[NUM_MONITORS + 1];
    SDL_FRect rects[NUM_MONITORS + 1];
    const int n = Screen_GetPictures(tex, rects, NUM_MONITORS + 1);
    for (int i = 0; i < n; i++) {
        const SDL_FRect px{(s.dest.x + rects[i].x * s.dest_scale) * fb.x, (s.dest.y + rects[i].y * s.dest_scale) * fb.y,
                           rects[i].w * s.dest_scale * fb.x, rects[i].h * s.dest_scale * fb.y};
        // Whole-number scaling (1120x832 doubled on Retina) stays sharp.
        const float k = s.dest_scale * fb.x;
        SDL_SetTextureScaleMode(tex[i], std::fabs(k - std::round(k)) < 0.01f ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
        SDL_RenderTexture(s.renderer, tex[i], nullptr, &px);
    }
}

}  // namespace

State& st() { return g_state; }

void post(std::function<void()> action) {
    st().posted.push_back(std::move(action));
    wake();
}

void wake(int n) {
    State& s = st();
    s.pending_frames = std::max(s.pending_frames, n);
}

void run_modal(const std::function<bool()>& still_open) {
    State& s = st();
    ++s.modal_depth;
    wake(3);
    while (!bQuitProgram && still_open()) GuiEvent_EventHandler();
    --s.modal_depth;
    wake(3);
}

std::string shortcut_keys(int shortcut) {
    if (shortcut < 0 || shortcut >= SHORTCUT_KEYS) return {};
    const int plain = ConfigureParams.Shortcut.withoutModifier[shortcut];
    const int with_mod = ConfigureParams.Shortcut.withModifier[shortcut];
    if (plain) {
        const char* name = Keymap_GetKeyName(plain);
        if (name && *name) return name;
    }
    if (with_mod) {
        const char* name = Keymap_GetKeyName(with_mod);
        if (name && *name) return std::string("Ctrl+") + kAlt + "+" + name;
    }
    return {};
}

namespace {

// Ctrl+Option + letter on macOS, where the shortcut is one.
char mac_key(int shortcut) {
    const auto k = static_cast<SDL_Keycode>(ConfigureParams.Shortcut.withModifier[shortcut]);
    if ((k >= SDLK_A && k <= SDLK_Z) || (k >= SDLK_0 && k <= SDLK_9)) return static_cast<char>(k);
    return 0;
}

MenuEntry action(const char* label, int shortcut, bool enabled = true, bool checked = false) {
    MenuEntry e = MenuEntry::item(label, [shortcut] { ShortCut_Run(shortcut); }, enabled, checked);
    e.key(shortcut_keys(shortcut), mac_key(shortcut));
    return e;
}

void show_screen(int slot) {
    ConfigureParams.Screen.nSingleModeSlot = slot;
    Statusbar_UpdateInfo();
    Gui_RequestRepaint();
}

}  // namespace

void toggle_turbo() {
    ConfigureParams.System.bFullSpeed = !ConfigureParams.System.bFullSpeed;
    Statusbar_AddMessage(ConfigureParams.System.bFullSpeed ? "Turbo on: as fast as the host can go, fast disks"
                                                           : "Turbo off: back to the set speed",
                         0);
    wake(2);
}

std::vector<MenuEntry> media_menu();      // settings.cpp
void open_settings_file();                // settings.cpp
void save_settings_file();                // settings.cpp

// The menus. Actions run from the main loop after the menu closes, and look
// up what they act on then.
std::vector<MenuEntry> menus() {
    using E = MenuEntry;
    std::vector<E> machine;
    machine.push_back(action("Settings...", SHORTCUT_OPTIONS).as(E::Role::Settings));
    machine.push_back(E::item("Open Settings File...", [] { open_settings_file(); }));
    machine.push_back(E::item("Save Settings As...", [] { save_settings_file(); }));
    machine.push_back(E::separator());
    machine.push_back(action("Pause", SHORTCUT_PAUSE, true, !bEmulationActive)
                          .tip("Stops the machine where it is until chosen again"));
    machine.push_back(E::item("Turbo", [] { toggle_turbo(); }, true, ConfigureParams.System.bFullSpeed)
                          .tip("Runs the machine as fast as the host can go, with fast disks, until chosen again"));
    machine.push_back(action("Cold Reset", SHORTCUT_COLDRESET)
                          .tip("Restarts the machine at once, as switching it off and on"));
    machine.push_back(E::separator());
    machine.push_back(action("68k Debugger", SHORTCUT_DEBUG_M68K)
                          .tip("The 68040 debugger, in the terminal PreviousTurbo was started from"));
    machine.push_back(action("i860 Debugger", SHORTCUT_DEBUG_I860)
                          .tip("The NeXTdimension's i860 debugger, in the terminal PreviousTurbo was started from"));
    machine.push_back(E::separator());
    machine.push_back(action("Quit", SHORTCUT_QUIT).as(E::Role::Quit));

    std::vector<E> view;
    view.push_back(action("Status Bar", SHORTCUT_STATUSBAR, true, ConfigureParams.Screen.bShowStatusbar));
    view.push_back(action("Title Bar", SHORTCUT_TITLEBAR, !bInFullScreen, ConfigureParams.Screen.bShowTitlebar));
    view.push_back(action("Full Screen", SHORTCUT_FULLSCREEN, true, bInFullScreen));
    view.push_back(E::separator());
    // Which screen the window shows (one screen at a time)
    {
        const bool single = ConfigureParams.Screen.nMode == SCREEN_SINGLE;
        const bool cube = ConfigureParams.System.nMachineType != NEXT_STATION;
        view.push_back(E::item("Main Display", [] { show_screen(0); }, single,
                               single && ConfigureParams.Screen.nSingleModeSlot == 0));
        for (int i = 0; i < ND_MAX_BOARDS; i++) {
            const int slot = i * 2 + 2;
            if (!cube || !ConfigureParams.Dimension.board[i].bEnabled) continue;
            view.push_back(E::item("NeXTdimension in Slot " + std::to_string(slot), [slot] { show_screen(slot); },
                                   single, single && ConfigureParams.Screen.nSingleModeSlot == slot));
        }
        if (view.size() > 5)
            view.push_back(action("Next Screen", SHORTCUT_DIMENSION, single && cube)
                               .tip("Shows the next display: the main one, then each NeXTdimension"));
    }
    view.push_back(E::separator());
    view.push_back(action("Capture Mouse", SHORTCUT_MOUSEGRAB, true, bGrabMouse));
    view.push_back(E::item("Capture Mouse on Entry",
                           [] { ConfigureParams.Mouse.bCaptureOnEntry = !ConfigureParams.Mouse.bCaptureOnEntry; }, true,
                           ConfigureParams.Mouse.bCaptureOnEntry)
                       .tip("Capture the mouse when the pointer moves onto the screen (once the system shows its pointer)"));
    view.push_back(E::separator());
    view.push_back(action("Save Screenshot", SHORTCUT_SCREENSHOT));
    view.push_back(action("Record Sound", SHORTCUT_RECORD, true, bRecordingAiff));
    view.push_back(action("Sound", SHORTCUT_SOUND, true, ConfigureParams.Sound.bEnableSound));

    std::vector<E> help;
    help.push_back(E::item("Keyboard and Mouse", [] { st().keys_open = true; }));
    help.push_back(E::item(std::string("About ") + kAppName, [] { st().about_open = true; }).as(E::Role::About));

    return {E::submenu("Machine", std::move(machine)), E::submenu("Media", media_menu()),
            E::submenu("View", std::move(view)), E::submenu("Help", std::move(help))};
}

std::string file_name(const std::string& path) {
    return path.empty() ? std::string() : std::filesystem::path(path).filename().string();
}

std::string folder_of(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    const auto dir = std::filesystem::absolute(path, ec).parent_path();
    return ec ? std::string() : dir.string() + "/";
}

#if !defined(__APPLE__)
// Elsewhere the menus are drawn in the window.
bool install_native_menus(std::function<std::vector<MenuEntry>()>, std::function<void(std::function<void()>)>) {
    return false;
}
#endif

void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip("%s", text);
}

}  // namespace ptgui

using namespace ptgui;

extern "C" {

void Gui_Init(SDL_Window* window, SDL_Renderer* renderer) {
    State& s = st();
    s.window = window;
    s.renderer = renderer;
    // Sizes are in window coordinates, scaled for the desktop's UI scale; on
    // a high-density screen (Retina) text is rasterised at the pixel density
    // and drawn at the coordinate size, so it stays sharp.
    s.density = std::max(1.0f, SDL_GetWindowPixelDensity(window));
    s.scale = std::clamp(SDL_GetWindowDisplayScale(window) / s.density, 1.0f, 4.0f);

    s.ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(s.ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(s.scale);
    ImFontConfig font;
    font.SizePixels = 13.0f * s.scale * s.density;
    io.Fonts->AddFontDefault(&font);
    io.FontGlobalScale = 1.0f / s.density;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    s.native_menus = install_native_menus([] { return menus(); }, post_from_menu);
    s.next_frame_ns = SDL_GetTicksNS();
}

void Gui_UnInit(void) {
    State& s = st();
    if (!s.ctx) return;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(s.ctx);
    s.ctx = nullptr;
}

float Gui_BarsHeight(void) {
    State& s = st();
    float scale = s.scale;
    if (!s.ctx) {
        scale = std::clamp(SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()), 1.0f, 4.0f);
    }
    int bars = ConfigureParams.Screen.bShowStatusbar ? 1 : 0;
#if !defined(__APPLE__)
    bars += 1;  // the menu bar (macOS has the system menu bar)
#endif
    return static_cast<float>(bars) * (13.0f + 6.0f) * scale;
}

bool Gui_HandleEvent(SDL_Event* event) {
    State& s = st();
    if (!s.ctx) return false;
    if (s.event_hook && s.event_hook(*event)) {
        wake(2);
        return true;
    }
    // The emulator's repaint, at each vertical blank, is not ImGui's: it
    // draws a frame only if the screen changed (Gui_Frame).
    if (event->type >= SDL_EVENT_USER && event->user.code == SPECIAL_EVENT_REPAINT) return false;
    const bool input = is_input(event->type);
    SDL_Window* from = SDL_GetWindowFromEvent(event);
    const bool other_window = from && from != s.window;
    const bool grabbed = bGrabMouse && SDL_GetWindowRelativeMouseMode(s.window);

    if (!other_window && (!input || !grabbed || s.modal_depth > 0)) {
        ImGui_ImplSDL3_ProcessEvent(event);
        // ImGui takes a press and its release in separate frames, then acts.
        wake(6);
    }
    if (event->type == SDL_EVENT_WINDOW_RESIZED || event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event->type == SDL_EVENT_WINDOW_EXPOSED)
        wake(2);
    if (!input) return false;
    if (s.modal_depth > 0) return true;  // a dialog is open: the machine is paused
    if (other_window) return false;      // NeXTdimension windows: all to the machine

    ImGuiIO& io = ImGui::GetIO();
    switch (event->type) {
        case SDL_EVENT_MOUSE_MOTION:
            return !grabbed && io.WantCaptureMouse;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (!grabbed && io.WantCaptureMouse) return true;
            s.buttons_down |= 1u << (event->button.button & 31);
            return false;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            // Only the releases of presses that went to the machine
            const uint32_t bit = 1u << (event->button.button & 31);
            if (s.buttons_down & bit) {
                s.buttons_down &= ~bit;
                return false;
            }
            return true;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            return !grabbed && io.WantCaptureMouse;
        case SDL_EVENT_KEY_DOWN: {
            // Typing into a field, or a menu is open
            if (!grabbed && (io.WantCaptureKeyboard || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)))
                return true;
            if (event->key.scancode < s.keys_down.size()) s.keys_down[event->key.scancode] = true;
            return false;
        }
        case SDL_EVENT_KEY_UP: {
            if (event->key.scancode < s.keys_down.size() && s.keys_down[event->key.scancode]) {
                s.keys_down[event->key.scancode] = false;
                return false;
            }
            return true;
        }
        default:
            return true;
    }
}

// The releases go through the event queue like the user's own, so they reach
// the machine in order after anything it was already sent.
void Gui_ReleaseHeld(void) {
    State& s = st();
    const SDL_WindowID id = s.window ? SDL_GetWindowID(s.window) : 0;
    for (int button = 0; button < 32; ++button) {
        if (!(s.buttons_down & (1u << button))) continue;
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
        ev.button.windowID = id;
        ev.button.button = static_cast<Uint8>(button);
        SDL_PushEvent(&ev);
    }
    for (size_t sc = 0; sc < s.keys_down.size(); ++sc) {
        if (!s.keys_down[sc]) continue;
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_EVENT_KEY_UP;
        ev.key.windowID = id;
        ev.key.scancode = static_cast<SDL_Scancode>(sc);
        ev.key.key = SDL_GetKeyFromScancode(ev.key.scancode, SDL_KMOD_NONE, false);
        ev.key.mod = SDL_KMOD_NONE;
        SDL_PushEvent(&ev);
    }
}

int Gui_WaitTimeMs(void) {
    State& s = st();
    if (!s.ctx) return 100;
    if (s.pending_frames > 0 || s.repaint || !s.posted.empty()) {
        const uint64_t now = SDL_GetTicksNS();
        if (now >= s.next_frame_ns) return 0;
        return static_cast<int>((s.next_frame_ns - now + 999999) / 1000000);
    }
    return 50;  // looks at the status bar's lights and messages
}

void Gui_RequestRepaint(void) {
    st().repaint = true;
}

void Gui_Frame(void) {
    State& s = st();
    if (!s.ctx) return;
    const uint64_t now = SDL_GetTicksNS();
    if (now < s.next_frame_ns) return;

    deliver_choices();
    // The status bar is looked at twenty times a second, and redrawn only
    // when what it shows has changed: an idle machine costs no frames.
    if (now >= s.next_status_ns) {
        s.next_status_ns = now + 50000000ull;
        if (ConfigureParams.Screen.bShowStatusbar && status_changed(now)) wake(1);
    }
    if (s.repaint) {
        s.repaint = false;
        if (Screen_UpdatePictures()) wake(1);
    }
    if (s.pending_frames <= 0) return;
    --s.pending_frames;
    s.next_frame_ns = now + kFrameNs;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    float top = 0, bottom = io.DisplaySize.y;
    if (menu_bar_shown()) {
        draw_menu();
        top = ImGui::GetFrameHeight();
    }
    if (ConfigureParams.Screen.bShowStatusbar) {
        bottom -= ImGui::GetFrameHeight();
        draw_status_bar();
    }
    layout(top, bottom);
    draw_settings();
    draw_dialogs();
    ImGui::Render();

    SDL_SetRenderDrawColor(s.renderer, 0, 0, 0, 255);
    SDL_RenderClear(s.renderer);
    draw_pictures(io.DisplayFramebufferScale);
    // ImGui draws in window coordinates and clips in pixels: SDL's render
    // scale turns the one into the other (2 on a high-density screen).
    SDL_SetRenderScale(s.renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s.renderer);
    SDL_SetRenderScale(s.renderer, 1.0f, 1.0f);
    SDL_RenderPresent(s.renderer);
}

void Gui_RunPosted(void) {
    State& s = st();
    if (s.posted.empty()) return;
    auto actions = std::move(s.posted);
    s.posted.clear();
    for (auto& a : actions) a();
}

void Gui_WindowToScreen(float* x, float* y) {
    State& s = st();
    if (!s.ctx) return;
    if (s.dest.w <= 0) layout_now();
    *x = (*x - s.dest.x) / s.dest_scale;
    *y = (*y - s.dest.y) / s.dest_scale;
}

void Gui_ScreenToWindow(float* x, float* y) {
    State& s = st();
    if (!s.ctx) return;
    layout_now();
    *x = s.dest.x + *x * s.dest_scale;
    *y = s.dest.y + *y * s.dest_scale;
}

bool Gui_ModalActive(void) {
    return st().modal_depth > 0;
}

}  // extern "C"
