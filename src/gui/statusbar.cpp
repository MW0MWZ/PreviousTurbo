/*
  PreviousTurbo - statusbar.cpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The status bar at the foot of the window: the drive and network lights,
  the System, DSP and NeXTdimension lights, the speed, messages, and the
  machine's description. The Statusbar_* functions are called from the emulator's
  threads.
*/

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "gui_internal.h"

extern "C" {
#include "main.h"
#include "configuration.h"
#include "statusbar.h"
#include "m68000.h"
#include "timing.h"
}

namespace ptgui {

namespace {

struct Message {
    std::string text;
    uint32_t msecs;
    uint64_t until_ns = 0;  // set when first shown
};

std::atomic<uint64_t> g_led_until[NUM_DEVICE_LEDS];  // lit until (ns)
std::atomic<bool> g_system_led{false};
std::atomic<bool> g_dsp_led{false};
std::atomic<int> g_nd_led{0};
std::mutex g_mutex;               // guards the two below
std::deque<Message> g_messages;   // newest first, as the old status bar showed them
std::string g_info;               // the machine's description
std::atomic<bool> g_info_stale{true};
std::string g_shown;              // what the bar showed last (to see changes)

bool lit(drive_index_t led, uint64_t now) {
    return now < g_led_until[led].load(std::memory_order_relaxed);
}

// The message showing now, if any (messages time out one after another).
std::string current_message(uint64_t now) {
    std::lock_guard<std::mutex> lock(g_mutex);
    while (!g_messages.empty()) {
        Message& m = g_messages.front();
        if (!m.until_ns) m.until_ns = now + static_cast<uint64_t>(m.msecs) * 1000000ull;
        if (now < m.until_ns) return m.text;
        g_messages.pop_front();
    }
    return {};
}

std::string info() {
    if (g_info_stale.exchange(false)) {
        char buffer[100];
        Configuration_SetInfoString(buffer, sizeof buffer);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_info = buffer;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_info;
}

// How many times as fast as the real CPU the machine runs while it is busy:
// the cycles run against the host's time, less the time waited for real time
// and the time the CPU spent stopped. Sampled twice a second, each sample
// weighing half as much as the one after it.
struct SpeedSample {
    uint64_t at = 0, cycles = 0, waited = 0, stopped = 0;
    double c = 0, t = 0;  // decayed sums of cycles and busy seconds
    std::string text = "Speed  -  ";
};
SpeedSample g_speed;

const std::string& speed_text(uint64_t now) {
    SpeedSample& s = g_speed;
    if (!bEmulationActive) {
        s.at = 0;  // the pause is not measured
        return s.text;
    }
    if (s.at && now - s.at < 500000000ULL) return s.text;
    const uint64_t cycles = M68000_CyclesRun, waited = Timing_WaitedUs, stopped = M68000_StoppedUs;
    if (s.at) {
        const double busy = (now - s.at) / 1e9 - (waited - s.waited) / 1e6 - (stopped - s.stopped) / 1e6;
        s.c = s.c * 0.5 + static_cast<double>(cycles - s.cycles);
        s.t = s.t * 0.5 + std::max(busy, 0.0);
        if (s.t > 0.02 && s.c > 0) {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "Speed %4.1fx", s.c / (s.t * ConfigureParams.System.nCpuFreq * 1e6));
            s.text = buffer;
        }
    }
    s.at = now;
    s.cycles = cycles;
    s.waited = waited;
    s.stopped = stopped;
    return s.text;
}

}  // namespace

bool status_changed(uint64_t now) {
    char sig[64];
    std::snprintf(sig, sizeof sig, "%d%d%d%d %d%d%d %d", lit(DEVICE_LED_ENET, now), lit(DEVICE_LED_OD, now),
                  lit(DEVICE_LED_SCSI, now), lit(DEVICE_LED_FD, now), g_system_led.load(), g_dsp_led.load(),
                  g_nd_led.load(), bEmulationActive * 2 + ConfigureParams.System.bFullSpeed);
    std::string shown = std::string(sig) + speed_text(now) + current_message(now) + info();
    if (shown == g_shown) return false;
    g_shown = std::move(shown);
    return true;
}

void draw_status_bar() {
    const State& s = st();
    const uint64_t now = SDL_GetTicksNS();
    ImGuiIO& io = ImGui::GetIO();
    const float h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - h));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s.scale, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
    auto light = [&](const char* label, bool on, ImU32 colour, const char* tip) {
        const float r = ImGui::GetTextLineHeight() * 0.32f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 c(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f);
        ImGui::GetWindowDrawList()->AddCircleFilled(c, r, on ? colour : IM_COL32(60, 60, 60, 255));
        ImGui::Dummy(ImVec2(r * 2, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0, 4 * s.scale);
        ImGui::TextUnformatted(label);
        if (tip) ImGui::SetItemTooltip("%s", tip);
        ImGui::SameLine(0, 14 * s.scale);
    };
    const ImU32 amber = IM_COL32(255, 170, 0, 255);
    light("Power", true, IM_COL32(60, 220, 60, 255), nullptr);  // the machine is always on
    light("SCSI", lit(DEVICE_LED_SCSI, now), amber, "SCSI disk activity");
    light("MO", lit(DEVICE_LED_OD, now), amber, "Magneto-optical disk activity");
    light("Floppy", lit(DEVICE_LED_FD, now), amber, "Floppy disk activity");
    light("Net", lit(DEVICE_LED_ENET, now), IM_COL32(60, 200, 255, 255), "Ethernet frames sent or received");
    light("System", g_system_led.load(), IM_COL32(230, 50, 40, 255), "The system's LED (set by the operating system)");
    light("DSP", g_dsp_led.load(), IM_COL32(60, 110, 255, 255), "The DSP56001 is running");
    {
        const int nd = g_nd_led.load();
        light("ND", nd != 0, nd == 2 ? IM_COL32(230, 50, 40, 255) : IM_COL32(60, 110, 255, 255),
              "The NeXTdimension's i860 is running (red: in its CS8 mode)");
    }
    ImGui::TextUnformatted(speed_text(now).c_str());
    ImGui::SetItemTooltip("How many times as fast as a real %d MHz CPU it runs when busy, by the host's\n"
                          "measured speed", ConfigureParams.System.nCpuFreq);
    ImGui::SameLine(0, 14 * s.scale);
    // Turbo: as fast as the host can go until pressed again
    {
        const bool on = ConfigureParams.System.bFullSpeed;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * s.scale, 0));
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 60, 40, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(225, 80, 55, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(170, 45, 30, 255));
        }
        if (ImGui::SmallButton("Turbo")) post(toggle_turbo);
        if (on) ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        ImGui::SetItemTooltip("Run flat out: no speed limit, idle time skipped and fast disks, until pressed\n"
                              "again. The machine's clock runs ahead of real time meanwhile.");
        ImGui::SameLine(0, 14 * s.scale);
    }
    if (!bEmulationActive) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Paused");
        ImGui::SameLine(0, 14 * s.scale);
    }
    const std::string message = current_message(now);
    if (!message.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", message.c_str());
        ImGui::SameLine();
    }
    const std::string text = info();
    const float tw = ImGui::CalcTextSize(text.c_str()).x;
    const float x = ImGui::GetWindowWidth() - tw - 8 * s.scale;
    if (x > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(x);
        ImGui::TextDisabled("%s", text.c_str());
    } else {
        ImGui::NewLine();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace ptgui

using namespace ptgui;

extern "C" {

void Statusbar_BlinkLed(drive_index_t drive) {
    // Lights are shown for half a second after the last activity.
    if (drive >= 0 && drive < NUM_DEVICE_LEDS)
        g_led_until[drive].store(SDL_GetTicksNS() + 500000000ull, std::memory_order_relaxed);
}

void Statusbar_SetSystemLed(bool state) {
    g_system_led.store(state);
}

void Statusbar_SetDspLed(bool state) {
    g_dsp_led.store(state);
}

void Statusbar_SetNdLed(int state) {
    g_nd_led.store(state);
}

void Statusbar_UpdateInfo(void) {
    g_info_stale.store(true);
}

void Statusbar_AddMessage(const char* msg, uint32_t msecs) {
    if (!ConfigureParams.Screen.bShowStatusbar) return;  // no sense in queuing messages that aren't shown
    std::lock_guard<std::mutex> lock(g_mutex);
    // Shown for 2.5 s by default; a newer message shows first.
    g_messages.push_front(Message{msg ? msg : "", msecs ? msecs : 2500});
}

}  // extern "C"
