/*
  PreviousTurbo - dialogs.cpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Message boxes, About, the keyboard and mouse help, and the dialogs asking
  for a missing ROM, disk image or folder. Those the emulator waits for keep
  the window alive until they are answered.
*/

#include <memory>

#include "gui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

extern "C" {
#include "main.h"
#include "configuration.h"
#include "dialog.h"
#include "event.h"
#include "file.h"
#include "keymap.h"
#include "paths.h"
#include "screen.h"
}

namespace ptgui {

namespace {

// A dialog drawn as a modal popup; dialogs opened while one is showing
// appear over it.
struct Modal {
    std::string title;
    std::function<void(Modal&)> body;  // draws the contents; sets `done` to close
    bool opened = false;
    bool done = false;
};

std::vector<std::shared_ptr<Modal>> g_modals;

void push(std::shared_ptr<Modal> m) {
    g_modals.push_back(std::move(m));
    wake(3);
}

// Shows a dialog and waits until it is closed.
void show_and_wait(const std::shared_ptr<Modal>& m) {
    push(m);
    run_modal([m] { return !m->done; });
    if (!m->done) m->done = true;  // quitting: closed with the program
}

void draw_modal(std::size_t i) {
    if (i >= g_modals.size()) return;
    Modal& m = *g_modals[i];
    const std::string id = m.title + "##modal" + std::to_string(i);
    if (!m.opened) {
        ImGui::OpenPopup(id.c_str());
        m.opened = true;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        m.body(m);
        if (m.done) ImGui::CloseCurrentPopup();
        else draw_modal(i + 1);
        ImGui::EndPopup();
    } else {
        m.done = true;  // closed some other way
    }
}

float button_w() { return 90 * st().scale; }

void wrapped(const std::string& text) {
    ImGui::PushTextWrapPos(460 * st().scale);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
}

bool alert(const std::string& text, bool query) {
    // Not shown with the mouse locked into the window
    const bool grabbed = bGrabMouse;
    if (grabbed) Screen_SetMouseGrab(false);
    Screen_ShowMainWindow();
    bool ok = false;
    auto m = std::make_shared<Modal>();
    m->title = kAppName;
    m->body = [text, query, &ok](Modal& self) {
        wrapped(text);
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(button_w(), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            ok = true;
            self.done = true;
        }
        if (query) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(button_w(), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) self.done = true;
        }
    };
    show_and_wait(m);
    if (grabbed) Screen_SetMouseGrab(bGrabMouse);
    return ok || !query;
}

// A request from another thread, run on the main thread.
struct Request {
    std::function<void()> run;
    SDL_Semaphore* done = nullptr;  // signalled after running, if waited for
};

void run_on_main(std::function<void()> fn, bool wait) {
    auto* req = new Request{std::move(fn), wait ? SDL_CreateSemaphore(0) : nullptr};
    SDL_Semaphore* done = req->done;
    GuiEvent_SendGuiRequest(req);
    if (done) {
        SDL_WaitSemaphore(done);
        SDL_DestroySemaphore(done);
    }
}

// A path with Browse... (and Default) buttons, editing a configuration string.
void path_row(const char* id, char* path, std::size_t size, Pick kind, const char* default_path,
              std::vector<FileFilter> filters = {}) {
    ImGui::PushID(id);
    std::string text = path;
    ImGui::SetNextItemWidth(360 * st().scale);
    if (ImGui::InputText("##path", &text)) SDL_strlcpy(path, text.c_str(), size);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        choose(kind, path, std::move(filters), [path, size](const std::string& chosen) {
            if (!chosen.empty()) SDL_strlcpy(path, chosen.c_str(), size);
        });
    }
    if (default_path) {
        ImGui::SameLine();
        if (ImGui::Button("Default")) SDL_strlcpy(path, default_path, size);
    }
    ImGui::PopID();
}

}  // namespace

void draw_dialogs() {
    State& s = st();
    if (s.about_open) {
        s.about_open = false;
        auto m = std::make_shared<Modal>();
        m->title = std::string("About ") + kAppName;
        m->body = [](Modal& self) {
            ImGui::Text("%s: a NeXT computer emulator", kAppName);
            ImGui::TextUnformatted("Based on " PROG_NAME_PREVIOUS ", the NeXT computer system (black hardware)");
            ImGui::TextUnformatted("emulator written by Andreas Grabher, Simon Schubiger and Gilles F\xc3\xa9tis");
            ImGui::TextUnformatted("with the help of many others. See readme.previous.txt for details.");
            ImGui::Spacing();
            ImGui::TextUnformatted("This program is free software: you can redistribute it and/or modify it under");
            ImGui::TextUnformatted("the GNU General Public License, version 2 or later. Please check the licence");
            ImGui::TextUnformatted("and copyright in the individual source files. It is distributed in the hope");
            ImGui::TextUnformatted("that it will be useful, but WITHOUT ANY WARRANTY.");
            ImGui::Spacing();
            ImGui::TextDisabled("Menus and dialogs: Dear ImGui (MIT licence)");
            ImGui::TextDisabled("NeXT, NeXTSTEP and OPENSTEP are trademarks of their owners.");
            if (ImGui::Button("OK", ImVec2(button_w(), 0))) self.done = true;
        };
        push(m);
    }
    if (s.keys_open) {
        s.keys_open = false;
        auto m = std::make_shared<Modal>();
        m->title = "Keyboard and Mouse";
        m->body = [](Modal& self) {
            static const char* const kNames[SHORTCUT_KEYS] = {
                "Settings", "Full screen on/off", "Lock/unlock mouse", "Cold reset", "Save screenshot",
                "Sound recording on/off", "Sound on/off", "68k debugger", "i860 debugger", "Pause",
                "Quit", "Next screen", "Status bar on/off", "Title bar on/off"};
            if (ConfigureParams.Mouse.bEnableAutoGrab) {
                ImGui::TextUnformatted("Click the screen to lock the mouse into the window for the NeXT;");
                ImGui::TextUnformatted("Ctrl-click releases it.");
            } else {
                ImGui::TextUnformatted("Lock the mouse into the window with View > Capture Mouse.");
            }
            ImGui::Spacing();
            if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) {
                for (int i = 0; i < SHORTCUT_KEYS; i++) {
                    const int plain = ConfigureParams.Shortcut.withoutModifier[i];
                    const int mod = ConfigureParams.Shortcut.withModifier[i];
                    std::string keys;
                    if (mod) keys = std::string("Ctrl+") + kAlt + "+" + Keymap_GetKeyName(mod);
                    if (plain) keys += (keys.empty() ? "" : "  or  ") + std::string(Keymap_GetKeyName(plain));
                    if (keys.empty()) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(keys.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kNames[i]);
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextDisabled("The keys can be changed in Settings > Keyboard.");
            if (ImGui::Button("OK", ImVec2(button_w(), 0))) self.done = true;
        };
        push(m);
    }
    draw_modal(0);
    // Forget dialogs that have closed (from the top down)
    while (!g_modals.empty() && g_modals.back()->done) g_modals.pop_back();
}

bool dialog_open() {
    return !g_modals.empty();
}

}  // namespace ptgui

using namespace ptgui;

extern "C" {

bool DlgAlert_Notice(const char* text) {
    const std::string t = text ? text : "";
    if (!SDL_IsMainThread()) {
        run_on_main([t] { alert(t, false); }, false);
        return true;
    }
    return alert(t, false);
}

bool DlgAlert_Query(const char* text) {
    const std::string t = text ? text : "";
    if (!SDL_IsMainThread()) {
        bool ok = false;
        run_on_main([t, &ok] { ok = alert(t, true); }, true);
        return ok;
    }
    return alert(t, true);
}

void Gui_HandleRequest(void* request) {
    std::unique_ptr<Request> req(static_cast<Request*>(request));
    if (req->run) req->run();
    if (req->done) SDL_SignalSemaphore(req->done);
}

void DlgMissing_Rom(const char* type, char* imgname, const char* defname, bool* enabled) {
    const std::string what = type;
    const std::string def = defname;
    auto m = std::make_shared<Modal>();
    m->title = "ROM file not found";
    m->body = [what, def, imgname, enabled](Modal& self) {
        ImGui::Text("%s: ROM file not found!", what.c_str());
        ImGui::TextUnformatted("Please select a valid ROM file:");
        path_row("rom", imgname, FILENAME_MAX, Pick::Open, def.c_str(),
                 {{"ROM images", "bin;BIN;rom"}, {"All files", "*"}});
        ImGui::Spacing();
        if (ImGui::Button("Select", ImVec2(button_w(), 0))) self.done = true;
        if (*enabled) {
            ImGui::SameLine();
            if (ImGui::Button("Remove", ImVec2(button_w(), 0))) {
                *enabled = false;
                imgname[0] = '\0';
                self.done = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(button_w(), 0))) {
            bQuitProgram = true;
            self.done = true;
        }
    };
    show_and_wait(m);
}

void DlgMissing_Disk(const char* type, int num, char* imgname, bool* ins, bool* wp) {
    const std::string what = type;
    auto m = std::make_shared<Modal>();
    m->title = "Disk image not found";
    m->body = [what, num, imgname, ins, wp](Modal& self) {
        ImGui::Text("%s drive %i: disk image not found!", what.c_str(), num);
        ImGui::TextUnformatted("Please remove or select a valid disk image:");
        path_row("disk", imgname, FILENAME_MAX, Pick::Open, nullptr);
        ImGui::Checkbox("Write protected", wp);
        ImGui::Spacing();
        if (ImGui::Button("Select", ImVec2(button_w(), 0))) self.done = true;
        ImGui::SameLine();
        if (ImGui::Button("Remove", ImVec2(button_w(), 0))) {
            *ins = false;
            *wp = false;
            imgname[0] = '\0';
            self.done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(button_w(), 0))) {
            bQuitProgram = true;
            self.done = true;
        }
    };
    show_and_wait(m);
}

void DlgMissing_Dir(const char* type, char* dirname, const char* defname) {
    const std::string what = type;
    const std::string def = defname;
    auto m = std::make_shared<Modal>();
    m->title = "Folder not found";
    m->body = [what, def, dirname](Modal& self) {
        ImGui::Text("%s directory not found!", what.c_str());
        ImGui::TextUnformatted("Please select a valid directory:");
        path_row("dir", dirname, FILENAME_MAX, Pick::Folder, def.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Select", ImVec2(button_w(), 0))) self.done = true;
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(button_w(), 0))) {
            bQuitProgram = true;
            self.done = true;
        }
    };
    show_and_wait(m);
}

}  // extern "C"
