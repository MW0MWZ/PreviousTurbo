/*
  PreviousTurbo - settings.cpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The Settings window (every option of the configuration the emulator
  reads), the Media menu, and opening and saving settings files.

  As with Previous's original dialogs, the Settings window edits
  ConfigureParams in place while the machine is paused; Dialog_DoProperty()
  keeps a copy to go back to on Cancel and applies the changes on OK.
  Inserting and ejecting disks happens at once, as on the real drives.
*/

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

extern "C" {
#include "main.h"
#include "configuration.h"
#include "change.h"
#include "dialog.h"
#include "file.h"
#include "floppy.h"
#include "keymap.h"
#include "log.h"
#include "mo.h"
#include "paths.h"
#include "scsi.h"
#include "shortcut.h"
#include "tablet.h"
#include "statusbar.h"
#if HAVE_PCAP
#include <pcap.h>
#endif
}
#include "dimension.hpp"

namespace ptgui {

namespace {

struct Session {
    bool open = false;
    bool ok = false;
    bool force_reset = false;
    // Mouse speeds as typed
    std::string lin_text, exp_text;
    // The shortcut being defined (-1: none) and which of its two keys
    int define = -1;
    bool define_with_mod = false;
    // The custom MAC address's last three bytes as typed
    std::string mac_text[3];
    int grid_cell = -1;  // the monitor grid's cell being chosen for
    bool mem_custom = false;  // memory set bank by bank
};
Session g;

const std::vector<FileFilter> kRomFilters = {{"ROM images", "bin;BIN;rom;ROM"}, {"All files", "*"}};
const std::vector<FileFilter> kDiskFilters = {{"Disk images", "img;dd;raw;hd;hdd;dsk;disk;diskimage"}, {"All files", "*"}};
const std::vector<FileFilter> kCdFilters = {{"CD-ROM images", "iso;img;bin;cdr;toast"}, {"All files", "*"}};
const std::vector<FileFilter> kMoFilters = {{"MO disk images", "img;dd;raw;mo;od"}, {"All files", "*"}};
const std::vector<FileFilter> kFloppyFilters = {{"Floppy images", "img;dd;raw;fd;flp"}, {"All files", "*"}};
const std::vector<FileFilter> kConfigFilters = {{"Settings", "cfg;ini;conf"}, {"All files", "*"}};

const char* const kEjectWarning =
    "WARNING: Don't eject manually if a guest system is running. Risk of data loss. Eject now?";

float S(float v) { return v * st().scale; }

void label(const char* text, float width = 0) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(width > 0 ? width : S(170));
}

void note(const char* text) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

void copy(char* dst, const std::string& src) {
    SDL_strlcpy(dst, src.c_str(), FILENAME_MAX);
}

std::string data_file(const char* name) {
    char buf[FILENAME_MAX];
    File_MakePathBuf(buf, sizeof buf, Paths_GetDataDir(), name, "BIN");
    return buf;
}

// An editable path with Browse... (and optionally Default) buttons.
void path_field(const char* id, char* path, Pick kind, const std::vector<FileFilter>& filters,
                const char* default_path = nullptr, float right = 0, Place place = Place::Disks) {
    ImGui::PushID(id);
    std::string text = path;
    const float buttons = S(default_path ? 150 : 80) + right;
    ImGui::SetNextItemWidth(std::max(S(120), ImGui::GetContentRegionAvail().x - buttons));
    if (ImGui::InputText("##path", &text)) copy(path, text);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        choose(kind, path, filters, [path](const std::string& chosen) {
            if (!chosen.empty()) copy(path, chosen);
        }, place);
    }
    if (default_path) {
        ImGui::SameLine();
        if (ImGui::Button("Default")) copy(path, default_path);
    }
    ImGui::PopID();
}

// --- Machine -----------------------------------------------------------------

struct MachineChoice {
    const char* label;
    MACHINETYPE type;
    bool turbo, color;
};
const MachineChoice kMachines[] = {
    {"NeXT Computer (68030)", NEXT_CUBE030, false, false},
    {"NeXTcube", NEXT_CUBE040, false, false},
    {"NeXTcube Turbo", NEXT_CUBE040, true, false},
    {"NeXTstation", NEXT_STATION, false, false},
    {"NeXTstation Turbo", NEXT_STATION, true, false},
    {"NeXTstation Color", NEXT_STATION, false, true},
    {"NeXTstation Turbo Color", NEXT_STATION, true, true},
};

int machine_index() {
    const CNF_SYSTEM& s = ConfigureParams.System;
    for (int i = 0; i < static_cast<int>(SDL_arraysize(kMachines)); i++) {
        const MachineChoice& m = kMachines[i];
        if (m.type != s.nMachineType) continue;
        if (m.type == NEXT_CUBE030) return i;
        if (m.turbo == s.bTurbo && (m.type == NEXT_CUBE040 || m.color == s.bColor)) return i;
    }
    return 0;
}

struct MemPreset {
    int mb;
    int banks[4];
};

// The memory sizes each machine offers, with the banks they use.
std::vector<MemPreset> memory_presets() {
    const CNF_SYSTEM& s = ConfigureParams.System;
    const bool wide = s.bColor || s.bTurbo;  // 8 MB modules
    std::vector<MemPreset> p;
    p.push_back(wide ? MemPreset{8, {8, 0, 0, 0}} : MemPreset{8, {4, 4, 0, 0}});
    p.push_back(wide ? MemPreset{16, {8, 8, 0, 0}} : MemPreset{16, {16, 0, 0, 0}});
    p.push_back(wide ? MemPreset{32, {8, 8, 8, 8}} : MemPreset{32, {16, 16, 0, 0}});
    if (s.bTurbo) {
        p.push_back({64, {32, 32, 0, 0}});
        p.push_back({128, {32, 32, 32, 32}});
    } else if (!s.bColor && s.nMachineType != NEXT_STATION) {
        p.push_back({64, {16, 16, 16, 16}});
    }
    return p;
}

// The presets Previous recognised as such (any other bank layout is custom).
bool is_known_layout(const int* banks) {
    static const int kKnown[][4] = {{4, 4, 0, 0},   {8, 0, 0, 0},     {16, 0, 0, 0},  {8, 8, 0, 0},   {16, 16, 0, 0},
                                    {8, 8, 8, 8},   {16, 16, 16, 16}, {32, 32, 0, 0}, {32, 32, 32, 32}};
    const CNF_SYSTEM& s = ConfigureParams.System;
    const int rows = s.bTurbo ? 9 : (s.bColor || s.nMachineType == NEXT_STATION) ? 6 : 8;
    for (int r = 0; r < rows; r++)
        if (!std::memcmp(kKnown[r], banks, sizeof kKnown[r])) return true;
    return false;
}

void tab_machine() {
    CNF_SYSTEM& sys = ConfigureParams.System;
    CNF_MEMORY& mem = ConfigureParams.Memory;

    label("Machine");
    ImGui::SetNextItemWidth(S(260));
    const int current = machine_index();
    if (ImGui::BeginCombo("##machine", kMachines[current].label)) {
        for (int i = 0; i < static_cast<int>(SDL_arraysize(kMachines)); i++) {
            if (ImGui::Selectable(kMachines[i].label, i == current) && i != current) {
                sys.nMachineType = kMachines[i].type;
                sys.bTurbo = kMachines[i].turbo;
                sys.bColor = kMachines[i].color;
                Configuration_SetSystemDefaults();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("System Defaults")) {
        sys.bRealtime = false;
        Configuration_SetSystemDefaults();
    }
    ImGui::SetItemTooltip("Sets the processor, clock, memory, DSP and chips this machine came with");
    note("Choosing a machine sets its processor, clock, FPU, DSP, memory and chips to how it came.");
    ImGui::Separator();

    label("CPU clock");
    {
        static const int kClocks[] = {16, 20, 25, 33, 40};
        for (int mhz : kClocks) {
            if (mhz == 40 && !sys.bTurbo) continue;
            char text[16];
            std::snprintf(text, sizeof text, "%d MHz", mhz);
            if (ImGui::RadioButton(text, sys.nCpuFreq == mhz)) sys.nCpuFreq = mhz;
            ImGui::SameLine();
        }
        ImGui::Checkbox("Variable", &sys.bRealtime);
        ImGui::SetItemTooltip("Runs the CPU as fast as the host can, with the guest's clocks following real time");
    }

    label("Speed");
    ImGui::Checkbox("Turbo", &sys.bFullSpeed);
    ImGui::SetItemTooltip("As fast as the host can go: no speed limit and fast disks, with the machine's\n"
                          "clocks kept to real time (the host sleeps while it is idle). Also in the status bar.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!sys.bFullSpeed);
    ImGui::Checkbox("JIT", &sys.bJIT);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("With Turbo, the 68040's code is compiled for this computer's processor,\n"
                          "which is much faster again. Off: Turbo runs the 68040 interpreter.");

    label("Memory size");
    {
        int banks[4];
        std::memcpy(banks, mem.nMemoryBankSize, sizeof banks);
        const int total = Configuration_CheckMemory(banks);
        const bool known = is_known_layout(banks) && !std::memcmp(banks, mem.nMemoryBankSize, sizeof banks);
        const auto presets = memory_presets();
        bool& custom = g.mem_custom;
        if (!known) custom = true;
        char shown[32];
        std::snprintf(shown, sizeof shown, custom ? "Custom (%d MB)" : "%d MB", total);
        ImGui::SetNextItemWidth(S(140));
        if (ImGui::BeginCombo("##memsize", shown)) {
            for (const MemPreset& p : presets) {
                char text[16];
                std::snprintf(text, sizeof text, "%d MB", p.mb);
                const bool sel = !custom && !std::memcmp(p.banks, mem.nMemoryBankSize, sizeof p.banks);
                if (ImGui::Selectable(text, sel)) {
                    std::memcpy(mem.nMemoryBankSize, p.banks, sizeof p.banks);
                    custom = false;
                }
            }
            if (ImGui::Selectable("Custom", custom)) custom = true;
            ImGui::EndCombo();
        }
        if (custom) {
            static const int kBankSizes[] = {2, 4, 8, 16, 32, 0};
            for (int b = 0; b < 4; b++) {
                ImGui::SetCursorPosX(S(170));
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Bank %d", b);
                ImGui::SameLine();
                ImGui::PushID(b);
                ImGui::SetNextItemWidth(S(100));
                char text[16];
                if (mem.nMemoryBankSize[b]) std::snprintf(text, sizeof text, "%d MB", mem.nMemoryBankSize[b]);
                else std::snprintf(text, sizeof text, "none");
                if (ImGui::BeginCombo("##bank", text)) {
                    for (int v : kBankSizes) {
                        char t[16];
                        if (v) std::snprintf(t, sizeof t, "%d MB", v);
                        else std::snprintf(t, sizeof t, "none");
                        if (ImGui::Selectable(t, mem.nMemoryBankSize[b] == v)) mem.nMemoryBankSize[b] = v;
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            ImGui::SetCursorPosX(S(170));
            if (ImGui::Button("Check")) Configuration_CheckMemory(mem.nMemoryBankSize);
            ImGui::SetItemTooltip("Corrects the banks to sizes this machine can take");
            ImGui::SetCursorPosX(S(170));
            ImGui::BeginGroup();
            note("Some values are only valid on certain machines; they are corrected when the settings are "
                 "applied. For booting Bank 0 must contain at least 4 MB of memory.");
            ImGui::EndGroup();
        }
    }

    label("Memory speed");
    {
        // The same four settings; Turbo machines name them by their faster parts.
        static const char* const kNormal[] = {"120 ns", "100 ns", "80 ns", "60 ns"};
        static const char* const kTurbo[] = {"60 ns", "70 ns", "80 ns", "100 ns"};
        const char* const* names = sys.bTurbo ? kTurbo : kNormal;
        if (mem.nMemorySpeed < MEMORY_120NS || mem.nMemorySpeed > MEMORY_60NS) mem.nMemorySpeed = MEMORY_100NS;
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(names[i], mem.nMemorySpeed == i)) mem.nMemorySpeed = static_cast<MEMORY_SPEED>(i);
        }
    }

    label("DSP");
    {
        if (ImGui::RadioButton("None##dsp", sys.nDSPType == DSP_TYPE_NONE)) sys.nDSPType = DSP_TYPE_NONE;
        ImGui::SameLine();
        if (ImGui::RadioButton("56001", sys.nDSPType != DSP_TYPE_NONE) && sys.nDSPType == DSP_TYPE_NONE)
            sys.nDSPType = DSP_TYPE_EMU;
        ImGui::SameLine();
        bool use_rom = sys.nDSPType == DSP_TYPE_ACCURATE;
        if (ImGui::Checkbox("Use ROM", &use_rom)) sys.nDSPType = use_rom ? DSP_TYPE_ACCURATE : DSP_TYPE_EMU;
        ImGui::SameLine(0, S(24));
        ImGui::TextUnformatted("Memory");
        ImGui::SameLine();
        if (ImGui::RadioButton("24 kB", !sys.bDSPMemoryExpansion)) sys.bDSPMemoryExpansion = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("96 kB", sys.bDSPMemoryExpansion)) sys.bDSPMemoryExpansion = true;
    }
    label("SCSI chip");
    if (ImGui::RadioButton("NCR53C90", sys.nSCSI == NCR53C90)) sys.nSCSI = NCR53C90;
    ImGui::SameLine();
    if (ImGui::RadioButton("NCR53C90A", sys.nSCSI == NCR53C90A)) sys.nSCSI = NCR53C90A;
    label("RTC chip");
    if (ImGui::RadioButton("MC68HC68T1", sys.nRTC == MC68HC68T1)) sys.nRTC = MC68HC68T1;
    ImGui::SameLine();
    if (ImGui::RadioButton("MCCS1850", sys.nRTC == MCCS1850)) sys.nRTC = MCCS1850;
    label("Other");
    ImGui::Checkbox("NBIC present", &sys.bNBIC);
    ImGui::SameLine();
    ImGui::Checkbox("ADB devices", &sys.bADB);
    note("ADB is only used by Turbo machines and NeXTstations have no NBIC: these are corrected on OK.");

    ImGui::Separator();
    // What the machine is, as the original System dialog showed it
    {
        static const char* const kCpu[] = {"68000", "68010", "68020", "68030", "68040", "68060"};
        const char* fpu = sys.n_FPUType == FPU_NONE    ? "none"
                          : sys.n_FPUType == FPU_68881 ? "68881"
                          : sys.n_FPUType == FPU_68882 ? "68882"
                                                       : "68040";
        int banks[4];
        std::memcpy(banks, mem.nMemoryBankSize, sizeof banks);
        char clock[16];
        if (sys.bRealtime) std::snprintf(clock, sizeof clock, "variable");
        else std::snprintf(clock, sizeof clock, "%d MHz", sys.nCpuFreq);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("CPU %s at %s, FPU %s, DSP %s with %s, %d MB memory, %s, %s, NBIC %s, ADB %s",
                            kCpu[std::clamp(sys.nCpuLevel, 0, 5)], clock, fpu,
                            sys.nDSPType == DSP_TYPE_NONE ? "none" : "56001", sys.bDSPMemoryExpansion ? "96 kB" : "24 kB",
                            Configuration_CheckMemory(banks), sys.nSCSI == NCR53C90 ? "NCR53C90" : "NCR53C90A",
                            sys.nRTC == MC68HC68T1 ? "MC68HC68T1" : "MCCS1850", sys.bNBIC ? "present" : "none",
                            sys.bADB ? "used" : (sys.bTurbo ? "unused" : "none"));
        ImGui::PopTextWrapPos();
    }
}

// --- Boot ----------------------------------------------------------------------

void tab_boot() {
    CNF_BOOT& b = ConfigureParams.Boot;
    label("Boot device");
    {
        static const struct {
            const char* label;
            BOOT_DEVICE dev;
        } kDevices[] = {{"SCSI disk", BOOT_SCSI}, {"Ethernet", BOOT_ETHERNET}, {"MO disk", BOOT_MO},
                        {"Floppy", BOOT_FLOPPY}, {"ROM monitor", BOOT_ROM}};
        ImGui::BeginGroup();
        for (const auto& d : kDevices)
            if (ImGui::RadioButton(d.label, b.nBootDevice == d.dev)) b.nBootDevice = d.dev;
        ImGui::EndGroup();
    }
    ImGui::Spacing();
    label("Power-on tests");
    ImGui::BeginGroup();
    ImGui::Checkbox("Perform DRAM test", &b.bEnableDRAMTest);
    if (ImGui::Checkbox("Perform power-on test", &b.bEnablePot) && !b.bEnablePot) {
        b.bEnableSoundTest = b.bEnableSCSITest = b.bLoopPot = b.bVerbose = false;
    }
    ImGui::Indent(S(20));
    // Any of these needs the power-on test
    if (ImGui::Checkbox("Sound out test", &b.bEnableSoundTest) && b.bEnableSoundTest) b.bEnablePot = true;
    if (ImGui::Checkbox("SCSI tests", &b.bEnableSCSITest) && b.bEnableSCSITest) b.bEnablePot = true;
    if (ImGui::Checkbox("Loop until keypress", &b.bLoopPot) && b.bLoopPot) b.bEnablePot = true;
    if (ImGui::Checkbox("Verbose test mode", &b.bVerbose) && b.bVerbose) b.bEnablePot = true;
    ImGui::Unindent(S(20));
    ImGui::Checkbox("Boot extended diagnostics", &b.bExtendedPot);
    ImGui::Checkbox("Show VRAM during test", &b.bVisible);
    ImGui::EndGroup();
    ImGui::Spacing();
    note("For advanced options boot to the ROM monitor prompt. Changes here reset the machine.");
}

// --- ROM -----------------------------------------------------------------------

void tab_rom() {
    CNF_ROM& r = ConfigureParams.Rom;
    const std::string d030 = data_file("Rev_1.0_v41"), d040 = data_file("Rev_2.5_v66"), dturbo = data_file("Rev_3.3_v74");
    ImGui::TextUnformatted("ROM for 68030 based systems:");
    path_field("rom030", r.szRom030FileName, Pick::Open, kRomFilters, d030.c_str(), 0, Place::Roms);
    ImGui::Spacing();
    ImGui::TextUnformatted("ROM for 68040 based systems:");
    path_field("rom040", r.szRom040FileName, Pick::Open, kRomFilters, d040.c_str(), 0, Place::Roms);
    ImGui::Spacing();
    ImGui::TextUnformatted("ROM for 68040 Turbo systems:");
    path_field("romturbo", r.szRomTurboFileName, Pick::Open, kRomFilters, dturbo.c_str(), 0, Place::Roms);
    ImGui::Spacing();
    note("A reset is needed after changing these options.");
}

// --- Display -------------------------------------------------------------------

// Warns when the console is on a NeXTdimension with more memory than it shows on.
void check_console(int slot) {
    post([slot] { Dialog_GraphicsCheckConsole(slot); });
}

void tab_display() {
    CNF_SCREEN& scr = ConfigureParams.Screen;
    CNF_ND& nd = ConfigureParams.Dimension;

    ImGui::TextUnformatted("NeXTdimension boards:");
    for (int n = 0; n < ND_MAX_BOARDS; n++) {
        NDBOARD& board = nd.board[n];
        ImGui::PushID(n);
        char text[48];
        std::snprintf(text, sizeof text, "Board in slot %d", ND_SLOT(n));
        bool enabled = board.bEnabled;
        if (ImGui::Checkbox(text, &enabled)) {
            if (enabled) {
                board.bEnabled = true;
                ConfigureParams.System.bNBIC = true;
                Configuration_SetDefaultScreen(ND_SLOT(n));
            } else {
                board.bEnabled = false;
            }
            check_console(ND_SLOT(n));
        }
        if (board.bEnabled) {
            ImGui::Indent(S(24));
            int banks[4];
            std::memcpy(banks, board.nMemoryBankSize, sizeof banks);
            ImGui::TextDisabled("i860XR at 33 MHz, %d MB memory, NBIC", Configuration_CheckDimensionMemory(banks));
            static const int kSizes[] = {4, 16, 0};
            for (int b = 0; b < 4; b++) {
                if (b) ImGui::SameLine();
                ImGui::PushID(b);
                ImGui::SetNextItemWidth(S(90));
                char cur[16];
                if (board.nMemoryBankSize[b]) std::snprintf(cur, sizeof cur, "%d MB", board.nMemoryBankSize[b]);
                else std::snprintf(cur, sizeof cur, "none");
                char lbl[16];
                std::snprintf(lbl, sizeof lbl, "Bank %d", b);
                if (ImGui::BeginCombo(lbl, cur)) {
                    for (int v : kSizes) {
                        char t[16];
                        if (v) std::snprintf(t, sizeof t, "%d MB", v);
                        else std::snprintf(t, sizeof t, "none");
                        if (ImGui::Selectable(t, board.nMemoryBankSize[b] == v)) board.nMemoryBankSize[b] = v;
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            ImGui::SameLine();
            if (ImGui::Button("Defaults")) {
                for (int b = 0; b < 4; b++) board.nMemoryBankSize[b] = 4;
                Configuration_CheckDimensionMemory(board.nMemoryBankSize);
            }
            note("Bank 0 must contain at least 4 MB of memory.");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("ROM");
            ImGui::SameLine();
            path_field("ndrom", board.szRomFileName, Pick::Open, kRomFilters, nullptr, 0, Place::Roms);
            ImGui::Unindent(S(24));
        }
        ImGui::PopID();
    }
    note("NeXTdimension does not work with NeXTstations.");
    ImGui::Separator();

    label("Show display");
    ImGui::BeginGroup();
    {
        const bool single = scr.nMode == SCREEN_SINGLE;
        if (ImGui::RadioButton("Main display (monochrome or colour)", single && scr.nSingleModeSlot == 0)) {
            scr.nMode = SCREEN_SINGLE;
            scr.nSingleModeSlot = 0;
        }
        for (int n = 0; n < ND_MAX_BOARDS; n++) {
            char text[48];
            std::snprintf(text, sizeof text, "NeXTdimension (slot %d)", ND_SLOT(n));
            if (ImGui::RadioButton(text, single && scr.nSingleModeSlot == ND_SLOT(n))) {
                scr.nMode = SCREEN_SINGLE;
                scr.nSingleModeSlot = ND_SLOT(n);
            }
        }
        if (ImGui::RadioButton("All, each in a separate window", scr.nMode == SCREEN_ALL)) {
            scr.nMode = SCREEN_ALL;
            scr.nSingleModeSlot = 0;
        }
        if (ImGui::RadioButton("All, grouped in a single window", scr.nMode == SCREEN_GROUP)) scr.nMode = SCREEN_GROUP;
    }
    ImGui::EndGroup();
    if (scr.nMode == SCREEN_GROUP) {
        label("Arrangement");
        ImGui::BeginGroup();
        static const char* const kMonitors[] = {"NeXT Main CPU (Slot 0)", "NeXTdimension (Slot 2)", "NeXTdimension (Slot 4)",
                                                "NeXTdimension (Slot 6)"};
        for (int pos = 0; pos < NUM_MONITORS * NUM_MONITORS; pos++) {
            if (pos % NUM_MONITORS) ImGui::SameLine();
            const int mon = Configuration_GetScreenFromPos(pos);
            char text[16];
            std::snprintf(text, sizeof text, "%s##cell%d", mon >= 0 ? std::to_string(mon).c_str() : "", pos);
            if (mon >= 0) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(text, ImVec2(S(36), S(28)))) {
                g.grid_cell = pos;
                ImGui::OpenPopup("##monitor");
            }
            if (mon >= 0) {
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s", kMonitors[mon]);
            }
        }
        if (ImGui::BeginPopup("##monitor")) {
            ImGui::TextDisabled("Show here:");
            for (int m = -1; m < NUM_MONITORS; m++) {
                if (ImGui::Selectable(m < 0 ? "None" : kMonitors[m])) {
                    const int pos = g.grid_cell;
                    const int occupant = Configuration_GetScreenFromPos(pos);
                    if (occupant >= 0) scr.nGroupModePos[occupant] = -1;
                    if (m >= 0) scr.nGroupModePos[m] = pos;
                    scr.nMode = SCREEN_GROUP;
                }
            }
            ImGui::EndPopup();
        }
        note("Monitor 0 is the main display, 1 to 3 the NeXTdimensions in slots 2, 4 and 6.");
        ImGui::EndGroup();
    }
    ImGui::Separator();

    label("Console");
    {
        bool on_nd = nd.nConsoleSlot > 0;
        if (ImGui::Checkbox("Console on NeXTdimension", &on_nd)) {
            nd.nConsoleSlot = on_nd ? 2 : 0;
            if (on_nd) check_console(nd.nConsoleSlot);
        }
        if (nd.nConsoleSlot > 0) {
            ImGui::SameLine();
            for (int n = 0; n < ND_MAX_BOARDS; n++) {
                char text[16];
                std::snprintf(text, sizeof text, "Slot %d", ND_SLOT(n));
                ImGui::SameLine();
                if (ImGui::RadioButton(text, nd.nConsoleSlot == ND_SLOT(n))) {
                    nd.nConsoleSlot = ND_SLOT(n);
                    check_console(nd.nConsoleSlot);
                }
            }
        }
    }
    label("Window");
    ImGui::Checkbox("Full screen", &scr.bFullScreen);
    note("The status bar and title bar are switched on and off in the View menu.");
}

// --- Storage -------------------------------------------------------------------

// Inserting and ejecting, at once, as the original dialogs and the Media menu do.
void insert_scsi(int i) {
    SCSIDISK& t = ConfigureParams.SCSI.target[i];
    const auto& filters = t.nDeviceType == SD_CD ? kCdFilters : t.nDeviceType == SD_FLOPPY ? kFloppyFilters : kDiskFilters;
    const std::string path = choose_now(Pick::Open, t.szImageName, filters);
    if (path.empty()) return;
    copy(t.szImageName, path);
    t.bDiskInserted = true;
    if (t.nDeviceType == SD_NONE) t.nDeviceType = SD_HARDDISK;
    if (t.nDeviceType != SD_HARDDISK) SCSI_Insert(static_cast<uint8_t>(i));
}

void eject_scsi(int i) {
    if (!DlgAlert_Query(kEjectWarning)) return;
    SCSIDISK& t = ConfigureParams.SCSI.target[i];
    t.bDiskInserted = false;
    t.szImageName[0] = '\0';
    SCSI_Eject(static_cast<uint8_t>(i));
}

bool connect_second_mo() {
    return DlgAlert_Query("WARNING: Second drive can cause problems. System may crash or hang due to a kernel bug. "
                          "Add anyway?");
}

void insert_mo(int n) {
    MODISK& d = ConfigureParams.MO.drive[n];
    if (n == 1 && !d.bDriveConnected && !connect_second_mo()) return;
    const std::string path = choose_now(Pick::Open, d.szImageName, kMoFilters);
    if (path.empty()) return;
    copy(d.szImageName, path);
    d.bDiskInserted = true;
    d.bDriveConnected = true;
    MO_Insert(n);
}

void eject_mo(int n) {
    if (!DlgAlert_Query(kEjectWarning)) return;
    MODISK& d = ConfigureParams.MO.drive[n];
    d.bDiskInserted = false;
    d.bWriteProtected = false;
    d.szImageName[0] = '\0';
    MO_Eject(n);
}

void insert_floppy(int n) {
    FLPDISK& d = ConfigureParams.Floppy.drive[n];
    const std::string path = choose_now(Pick::Open, d.szImageName, kFloppyFilters);
    if (path.empty()) return;
    copy(d.szImageName, path);
    if (Floppy_Insert(n)) {
        DlgAlert_Notice("ERROR: Can't insert floppy disk. Bad disk size. Must be 720K, 1440K or 2880K.");
        d.bWriteProtected = false;
        d.szImageName[0] = '\0';
        return;
    }
    d.bDiskInserted = true;
    d.bDriveConnected = true;
}

void eject_floppy(int n) {
    if (!DlgAlert_Query(kEjectWarning)) return;
    FLPDISK& d = ConfigureParams.Floppy.drive[n];
    d.bDiskInserted = false;
    d.bWriteProtected = false;
    d.szImageName[0] = '\0';
    Floppy_Eject(n);
}

void tab_storage() {
    CNF_SCSI& scsi = ConfigureParams.SCSI;
    ImGui::TextUnformatted("SCSI disks:");
    static const char* const kTypes[] = {"None", "Hard disk", "CD-ROM", "Floppy"};
    if (ImGui::BeginTable("##scsi", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, S(46));
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, S(100));
        ImGui::TableSetupColumn("Image", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("RO", ImGuiTableColumnFlags_WidthFixed, S(110));
        for (int i = 0; i < ESP_MAX_DEVS; i++) {
            SCSIDISK& t = scsi.target[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Disk %d", i);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            const int type = std::clamp(static_cast<int>(t.nDeviceType), 0, NUM_SD - 1);
            if (ImGui::BeginCombo("##type", kTypes[type])) {
                for (int k = 0; k < NUM_SD; k++)
                    if (ImGui::Selectable(kTypes[k], k == type)) t.nDeviceType = static_cast<SCSI_DEVTYPE>(k);
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            if (t.nDeviceType == SD_HARDDISK || t.nDeviceType == SD_NONE) {
                ImGui::BeginDisabled(t.nDeviceType == SD_NONE);
                // Only a disk that is in shows its file (unused names are kept as made absolute)
                std::string text = t.nDeviceType == SD_HARDDISK && t.bDiskInserted ? t.szImageName : "";
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(80));
                if (ImGui::InputText("##path", &text)) {
                    copy(t.szImageName, text);
                    t.bDiskInserted = !text.empty();
                }
                ImGui::SameLine();
                if (ImGui::Button("Browse...")) post([i] { insert_scsi(i); });
                ImGui::EndDisabled();
            } else {
                // CD-ROM and floppy: the disc is changed at once
                const std::string name = t.bDiskInserted ? file_name(t.szImageName) : "(empty)";
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(80));
                std::string shown = name;
                ImGui::InputText("##path", &shown, ImGuiInputTextFlags_ReadOnly);
                if (t.bDiskInserted) ImGui::SetItemTooltip("%s", t.szImageName);
                ImGui::SameLine();
                if (t.bDiskInserted) {
                    if (ImGui::Button("Eject", ImVec2(S(72), 0))) post([i] { eject_scsi(i); });
                } else if (ImGui::Button("Insert...", ImVec2(S(72), 0))) {
                    post([i] { insert_scsi(i); });
                }
            }
            ImGui::TableNextColumn();
            if (t.nDeviceType == SD_CD) {
                ImGui::BeginDisabled();
                bool ro = true;
                ImGui::Checkbox("Read-only", &ro);
                ImGui::EndDisabled();
            } else {
                ImGui::BeginDisabled(t.nDeviceType == SD_NONE);
                ImGui::Checkbox("Read-only", &t.bWriteProtected);
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    note("Hard disk changes reset the machine; CD-ROM and floppy discs change at once.");
    ImGui::Separator();

    ImGui::TextUnformatted("Magneto-optical drives:");
    for (int n = 0; n < MO_MAX_DRIVES; n++) {
        MODISK& d = ConfigureParams.MO.drive[n];
        ImGui::PushID(100 + n);
        char text[32];
        std::snprintf(text, sizeof text, "MO drive %d", n);
        bool connected = d.bDriveConnected;
        if (ImGui::Checkbox(text, &connected)) {
            if (!connected) {
                d.bDriveConnected = false;
                d.bDiskInserted = false;
                d.bWriteProtected = false;
                d.szImageName[0] = '\0';
            } else if (n == 1) {
                post([] {
                    if (connect_second_mo()) ConfigureParams.MO.drive[1].bDriveConnected = true;
                });
            } else {
                d.bDriveConnected = true;
            }
        }
        ImGui::SameLine(S(130));
        std::string shown = d.bDriveConnected && d.bDiskInserted ? file_name(d.szImageName) : "(no disk)";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(200));
        ImGui::InputText("##path", &shown, ImGuiInputTextFlags_ReadOnly);
        if (d.bDiskInserted) ImGui::SetItemTooltip("%s", d.szImageName);
        ImGui::SameLine();
        if (d.bDriveConnected && d.bDiskInserted) {
            if (ImGui::Button("Eject", ImVec2(S(72), 0))) post([n] { eject_mo(n); });
        } else if (ImGui::Button("Insert...", ImVec2(S(72), 0))) {
            post([n] { insert_mo(n); });
        }
        ImGui::SameLine();
        ImGui::Checkbox("Read-only", &d.bWriteProtected);
        ImGui::PopID();
    }
    note("Magneto-optical drives only work with non-turbo Cubes.");
    ImGui::Separator();

    ImGui::TextUnformatted("Floppy disk drive:");
    {
        // Real NeXT hardware has one drive
        FLPDISK& d = ConfigureParams.Floppy.drive[0];
        ImGui::PushID(200);
        bool connected = d.bDriveConnected;
        if (ImGui::Checkbox("Floppy drive 0", &connected)) {
            d.bDriveConnected = connected;
            if (!connected) {
                d.bDiskInserted = false;
                d.bWriteProtected = false;
                d.szImageName[0] = '\0';
            }
        }
        ImGui::SameLine(S(130));
        std::string shown = d.bDriveConnected && d.bDiskInserted ? file_name(d.szImageName) : "(no disk)";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(200));
        ImGui::InputText("##path", &shown, ImGuiInputTextFlags_ReadOnly);
        if (d.bDiskInserted) ImGui::SetItemTooltip("%s", d.szImageName);
        ImGui::SameLine();
        if (d.bDriveConnected && d.bDiskInserted) {
            if (ImGui::Button("Eject", ImVec2(S(72), 0))) post([] { eject_floppy(0); });
        } else if (ImGui::Button("Insert...", ImVec2(S(72), 0))) {
            post([] { insert_floppy(0); });
        }
        ImGui::SameLine();
        ImGui::Checkbox("Read-only", &d.bWriteProtected);
        ImGui::PopID();
    }
    note("Floppy disk drives do not work with 68030 based Cubes.");
}

// --- Network -------------------------------------------------------------------

// The MAC address in the machine's ROM (zeros if it cannot be read).
void rom_mac(uint8_t* mac) {
    const CNF_ROM& r = ConfigureParams.Rom;
    const char* name = ConfigureParams.System.nMachineType == NEXT_CUBE030 ? r.szRom030FileName
                       : ConfigureParams.System.bTurbo                     ? r.szRomTurboFileName
                                                                           : r.szRom040FileName;
    std::memset(mac, 0, 6);
    FILE* rom = File_Open(name, "rb");
    if (!rom) return;
    if (!File_Read(mac, 6, 8, rom)) std::memset(mac, 0, 6);
    fclose(rom);
}

#if HAVE_PCAP
struct PcapDevice {
    std::string name, shown;
};
std::vector<PcapDevice> pcap_devices() {
    std::vector<PcapDevice> list;
    pcap_if_t* all = nullptr;
    char err[PCAP_ERRBUF_SIZE];
    if (pcap_findalldevs(&all, err) == -1) {
        std::printf("Error in pcap_findalldevs: %s\n", err);
        return list;
    }
    for (pcap_if_t* d = all; d; d = d->next) {
#ifdef _WIN32
        list.push_back({d->name, d->description ? d->description : d->name});
#else
        list.push_back({d->name, d->name});
#endif
    }
    pcap_freealldevs(all);
    return list;
}
#endif

void tab_network() {
    CNF_ENET& e = ConfigureParams.Ethernet;
    CNF_ROM& r = ConfigureParams.Rom;

    label("Guest interface");
    ImGui::Checkbox("Ethernet connected", &e.bEthernetConnected);
    ImGui::SameLine(0, S(24));
    ImGui::BeginDisabled(!e.bEthernetConnected);
    if (ImGui::RadioButton("Thin wire", !e.bTwistedPair)) e.bTwistedPair = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Twisted pair", e.bTwistedPair)) e.bTwistedPair = true;
    ImGui::EndDisabled();

#if HAVE_PCAP
    label("Host interface");
    if (ImGui::RadioButton("SLiRP", e.nHostInterface == ENET_SLIRP)) e.nHostInterface = ENET_SLIRP;
    ImGui::SameLine();
    if (ImGui::RadioButton("PCAP", e.nHostInterface == ENET_PCAP)) {
        e.nHostInterface = ENET_PCAP;
        e.bNetworkTime = false;  // network time only works with SLiRP
    }
    if (e.nHostInterface == ENET_PCAP) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(220));
        static std::vector<PcapDevice> devices;
        if (ImGui::BeginCombo("##pcap", e.szInterfaceName[0] ? e.szInterfaceName : "(choose an interface)")) {
            if (ImGui::IsWindowAppearing()) devices = pcap_devices();
            if (devices.empty()) ImGui::TextDisabled("no interface found");
            for (const PcapDevice& d : devices)
                if (ImGui::Selectable(d.shown.c_str(), d.name == e.szInterfaceName)) copy(e.szInterfaceName, d.name);
            ImGui::EndCombo();
        }
    }
    ImGui::SetCursorPosX(S(170));
    if (ImGui::Checkbox("Network time", &e.bNetworkTime)) e.nHostInterface = ENET_SLIRP;
    ImGui::SetItemTooltip("The guest can set its clock from the host (SLiRP only)");
#else
    label("Host interface");
    ImGui::Checkbox("Use network time", &e.bNetworkTime);
#endif

    label("MAC address");
    {
        uint8_t mac[6];
        if (r.bUseCustomMac) for (int i = 0; i < 6; i++) mac[i] = static_cast<uint8_t>(r.nRomCustomMac[i]);
        else rom_mac(mac);
        bool custom = r.bUseCustomMac;
        // The first three bytes are the maker's; the last three can be set.
        ImGui::Text("%02x:%02x:%02x:", mac[0], mac[1], mac[2]);
        for (int i = 0; i < 3; i++) {
            ImGui::SameLine(0, i ? 2 : 0);
            ImGui::PushID(i);
            char text[3];
            std::snprintf(text, sizeof text, "%02x", mac[3 + i]);
            std::string edit = text;
            ImGui::SetNextItemWidth(S(26));
            ImGui::BeginDisabled(!custom);
            if (ImGui::InputText("##mac", &edit, ImGuiInputTextFlags_CharsHexadecimal) && edit.size() <= 2) {
                r.nRomCustomMac[3 + i] = static_cast<int>(std::strtol(edit.c_str(), nullptr, 16));
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            if (i < 2) {
                ImGui::SameLine(0, 2);
                ImGui::TextUnformatted(":");
            }
        }
        ImGui::SameLine(0, S(16));
        if (ImGui::Checkbox("Custom", &custom)) {
            if (custom) {
                for (int i = 0; i < 6; i++) r.nRomCustomMac[i] = mac[i];
            }
            r.bUseCustomMac = custom;
        }
        ImGui::SetItemTooltip("Off: the address programmed in the ROM");
    }
    ImGui::Separator();

    ImGui::TextUnformatted("NFS shared directories:");
    for (int i = 0; i < EN_MAX_SHARES; i++) {
        EN_NFS& share = e.nfs[i];
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(S(120));
        if (i == 0) {
            // The first share's name is fixed
            std::string fixed = "nfs";
            ImGui::InputText("##name", &fixed, ImGuiInputTextFlags_ReadOnly);
        } else {
            std::string name = share.szHostName;
            if (ImGui::InputText("##name", &name)) SDL_strlcpy(share.szHostName, name.c_str(), sizeof share.szHostName);
        }
        ImGui::SameLine();
        std::string path = share.szPathName;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(i ? 160 : 80));
        if (ImGui::InputText("##path", &path)) copy(share.szPathName, path);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            choose(Pick::Folder, share.szPathName, {}, [i](const std::string& chosen) {
                if (chosen.empty()) return;
                EN_NFS& s = ConfigureParams.Ethernet.nfs[i];
                copy(s.szPathName, chosen);
                if (i > 0 && !s.szHostName[0]) std::snprintf(s.szHostName, sizeof s.szHostName, "nfs%d", i);
            }, Place::Base);
        }
        if (i > 0) {
            ImGui::SameLine();
            ImGui::BeginDisabled(!share.szPathName[0]);
            if (ImGui::Button("Remove")) share.szPathName[0] = '\0';
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    note("The first NFS share cannot be renamed.");
#if HAVE_PCAP
    note("PCAP requires super user privileges.");
#endif
}

// --- Printer, Sound --------------------------------------------------------------

void tab_printer() {
    CNF_PRINTER& p = ConfigureParams.Printer;
    ImGui::Checkbox("Printer connected", &p.bPrinterConnected);
    label("File format");
    if (ImGui::RadioButton("PNG", p.nFileFormat == FORMAT_PNG)) p.nFileFormat = FORMAT_PNG;
    ImGui::SameLine();
    if (ImGui::RadioButton("TIFF", p.nFileFormat == FORMAT_TIFF)) p.nFileFormat = FORMAT_TIFF;
    label("Paper size");
    static const struct {
        const char* label;
        PAPER_SIZE size;
    } kPapers[] = {{"A4", PAPER_A4}, {"Letter", PAPER_LETTER}, {"B5", PAPER_B5}, {"Legal", PAPER_LEGAL}};
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(kPapers[i].label, p.nPaperSize == kPapers[i].size)) p.nPaperSize = kPapers[i].size;
    }
    ImGui::TextUnformatted("Directory to save printer output:");
    path_field("printdir", p.szPrintToFileName, Pick::Folder, {}, nullptr, 0, Place::Base);
}

void tab_sound() {
    ImGui::Checkbox("Sound enabled", &ConfigureParams.Sound.bEnableSound);
    ImGui::Checkbox("Microphone (sound input)", &ConfigureParams.Sound.bEnableMicrophone);
}

// --- Keyboard ----------------------------------------------------------------------

const char* const kShortcutNames[SHORTCUT_KEYS] = {
    "Settings", "Full screen on/off", "Lock/unlock mouse", "Cold reset", "Save screenshot", "Sound recording on/off",
    "Sound on/off", "68k debugger", "i860 debugger", "Pause", "Quit", "Next screen", "Status bar on/off",
    "Title bar on/off"};

// Waits for the key for shortcut `g.define`: a key sets it, the right button
// clears it, the left button gives up. Another shortcut with the key loses it.
void start_define(int sc, bool with_mod) {
    g.define = sc;
    g.define_with_mod = with_mod;
    st().event_hook = [](const SDL_Event& ev) {
        int key = -1;
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
            switch (ev.key.key) {
                case SDLK_LCTRL: case SDLK_RCTRL: case SDLK_LALT: case SDLK_RALT:
                case SDLK_LSHIFT: case SDLK_RSHIFT: case SDLK_LGUI: case SDLK_RGUI:
                    return true;  // the modifiers themselves
                default: key = static_cast<int>(ev.key.key); break;
            }
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (ev.button.button == SDL_BUTTON_RIGHT) key = 0;
            else {
                g.define = -1;
                st().event_hook = nullptr;
                return true;
            }
        } else {
            return ev.type == SDL_EVENT_KEY_UP || ev.type == SDL_EVENT_TEXT_INPUT;
        }
        int* table = g.define_with_mod ? ConfigureParams.Shortcut.withModifier : ConfigureParams.Shortcut.withoutModifier;
        table[g.define] = key;
        if (key) {
            int removed = 0;
            for (int i = 0; i < SHORTCUT_KEYS; i++)
                if (i != g.define && table[i] == key) {
                    table[i] = 0;
                    removed++;
                }
            for (int i = 0; i < removed; i++) post([] { DlgAlert_Notice("Removing key from other shortcut!"); });
        }
        g.define = -1;
        st().event_hook = nullptr;
        return true;
    };
}

void tab_keyboard() {
    CNF_KEYBOARD& k = ConfigureParams.Keyboard;
    label("Keyboard mapping");
    if (ImGui::RadioButton("Scancode", k.nKeymapType == KEYMAP_SCANCODE)) k.nKeymapType = KEYMAP_SCANCODE;
    ImGui::SetItemTooltip("Keys by their position on the keyboard");
    ImGui::SameLine();
    if (ImGui::RadioButton("Symbolic", k.nKeymapType == KEYMAP_SYMBOLIC)) k.nKeymapType = KEYMAP_SYMBOLIC;
    ImGui::SetItemTooltip("Keys by the character on them");
    label("Key options");
    ImGui::Checkbox("Swap Cmd and Alt", &k.bSwapCmdAlt);
    ImGui::Separator();
    ImGui::Text("Shortcuts: Ctrl+%s+key, or a key on its own", kAlt);
    if (ImGui::BeginTable("##shortcuts", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn((std::string("With Ctrl+") + kAlt).c_str(), ImGuiTableColumnFlags_WidthFixed, S(150));
        ImGui::TableSetupColumn("Without modifier", ImGuiTableColumnFlags_WidthFixed, S(150));
        ImGui::TableHeadersRow();
        for (int i = 0; i < SHORTCUT_KEYS; i++) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kShortcutNames[i]);
            for (int col = 0; col < 2; col++) {
                ImGui::TableNextColumn();
                ImGui::PushID(col);
                const bool with_mod = col == 0;
                const int key = with_mod ? ConfigureParams.Shortcut.withModifier[i] : ConfigureParams.Shortcut.withoutModifier[i];
                const bool waiting = g.define == i && g.define_with_mod == with_mod;
                std::string text = waiting ? "press a key..." : key ? Keymap_GetKeyName(key) : "-";
                if (ImGui::Button((text + "##def").c_str(), ImVec2(-1, 0)) && g.define < 0) start_define(i, with_mod);
                ImGui::SetItemTooltip("Click, then press the new key (right-click while waiting clears it)");
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// --- Mouse ---------------------------------------------------------------------------

void tab_mouse() {
    CNF_MOUSE& m = ConfigureParams.Mouse;
    // Previous's speed steps; any other value shows as custom.
    static const char* const kSpeeds[] = {"Very slow", "Slow", "Normal", "Fast", "Very fast", "Custom"};
    auto speed_row = [](const char* text, const char* id, float& value, float base, int first, float lo, float hi,
                        std::string& typed) {
        label(text);
        int sel = 5;
        for (int i = 0; i < 5; i++)
            if (std::fabs(value - base * static_cast<float>(first + i)) < 0.0001f) sel = i;
        ImGui::SetNextItemWidth(S(120));
        ImGui::PushID(id);
        if (ImGui::BeginCombo("##speed", kSpeeds[sel])) {
            for (int i = 0; i < 6; i++)
                if (ImGui::Selectable(kSpeeds[i], i == sel) && i < 5) value = base * static_cast<float>(first + i);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        char shown[16];
        std::snprintf(shown, sizeof shown, "%.3f", value);
        if (!ImGui::IsAnyItemActive()) typed = shown;
        ImGui::SetNextItemWidth(S(70));
        if (ImGui::InputText("##value", &typed, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            for (char& c : typed)
                if (c == ',') c = '.';
            float v = static_cast<float>(std::atof(typed.c_str()));
            if (!(v > 0)) v = 1.0f;
            value = std::clamp(v, lo, hi);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%.2f to %.2f)", lo, hi);
        ImGui::PopID();
    };
    speed_row("Slow movement", "lin", m.fLinScale, 0.125f, 6, static_cast<float>(MOUSE_LIN_MIN),
              static_cast<float>(MOUSE_LIN_MAX), g.lin_text);
    speed_row("Fast movement", "exp", m.fExpScale, 0.125f, 4, static_cast<float>(MOUSE_EXP_MIN),
              static_cast<float>(MOUSE_EXP_MAX), g.exp_text);
    ImGui::SetCursorPosX(S(170));
    ImGui::Checkbox("Use raw mouse movement data if available", &m.bUseRawMotion);
    ImGui::Separator();
    ImGui::Checkbox("Enable auto-locking (click the screen to lock the mouse)", &m.bEnableAutoGrab);
    ImGui::Checkbox("Map Ctrl-click to right-click", &m.bEnableMacClick);
    ImGui::Checkbox("Map scroll wheel to arrow keys", &m.bEnableMapToKey);
    ImGui::Separator();
    label("Tablet");
    {
        static const struct {
            const char* label;
            TABLET_TYPE type;
        } kTablets[] = {{"No tablet", TABLET_NONE},
                        {"WACOM SD-420E", TABLET_SD420E},
                        {"SummaGraphics MM 961", TABLET_MM961},
                        {"SummaGraphics MM 1201", TABLET_MM1201}};
        const char* current = "No tablet";
        for (const auto& t : kTablets)
            if (t.type == ConfigureParams.Tablet.nTabletType) current = t.label;
        ImGui::SetNextItemWidth(S(220));
        if (ImGui::BeginCombo("##tablet", current)) {
            for (const auto& t : kTablets) {
                if (ImGui::Selectable(t.label, t.type == ConfigureParams.Tablet.nTabletType) &&
                    t.type != ConfigureParams.Tablet.nTabletType) {
                    const TABLET_TYPE type = t.type;
                    post([type] {
                        if (bTabletEnabled && !DlgAlert_Query("Make sure tablet is disabled or uninstalled before "
                                                              "changing model. Save changes anyway?"))
                            return;
                        ConfigureParams.Tablet.nTabletType = type;
                    });
                }
            }
            ImGui::EndCombo();
        }
        note("With a tablet the pointer follows the host's (absolute positioning) when the guest has its driver.");
    }
}

// --- The window ------------------------------------------------------------------------

void load_config() {
    const std::string path = choose_now(Pick::Open, sConfigFileName, kConfigFilters, Place::Base);
    if (path.empty()) return;
    copy(sConfigFileName, path);
    Configuration_Load(nullptr);
}

void save_config() {
    const std::string path = choose_now(Pick::Save, sConfigFileName, kConfigFilters, Place::Base);
    if (path.empty()) return;
    copy(sConfigFileName, path);
    Configuration_Save();
}

}  // namespace

bool settings_open() { return g.open; }

void draw_settings() {
    if (!g.open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(S(700), S(560)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool keep = true;
    if (!ImGui::Begin("Settings", &keep, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        if (!keep) g.open = false;
        return;
    }
    if (!keep) g.open = false;  // closed: as Cancel

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.4f;
    ImGui::BeginChild("##tabs", ImVec2(0, -footer));
    if (ImGui::BeginTabBar("##settings", ImGuiTabBarFlags_FittingPolicyScroll)) {
        struct Tab {
            const char* name;
            void (*draw)();
        };
        static const Tab kTabs[] = {{"Machine", tab_machine}, {"Boot", tab_boot},       {"ROM", tab_rom},
                                    {"Display", tab_display}, {"Storage", tab_storage}, {"Network", tab_network},
                                    {"Printer", tab_printer}, {"Sound", tab_sound},     {"Keyboard", tab_keyboard},
                                    {"Mouse", tab_mouse}};
        for (const Tab& t : kTabs) {
            if (ImGui::BeginTabItem(t.name)) {
                ImGui::BeginChild("##page");
                t.draw();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::Checkbox("Reset the machine on OK", &g.force_reset);
    ImGui::SetItemTooltip("Restarts the machine when the settings are applied, whatever changed");
    ImGui::SameLine(0, S(20));
    ImGui::Checkbox("Show at startup", &ConfigureParams.ConfigDialog.bShowConfigDialogAtStartup);
    ImGui::SameLine(0, S(20));
    if (ImGui::Button("Load...")) post(load_config);
    ImGui::SetItemTooltip("Loads a settings file into this window (applied on OK)");
    ImGui::SameLine();
    if (ImGui::Button("Save As...")) post(save_config);
    ImGui::SetItemTooltip("Saves these settings to a file (%s now)", file_name(sConfigFileName).c_str());

    const float bw = S(90);
    if (ImGui::Button("OK", ImVec2(bw, 0)) || (ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsAnyItemActive())) {
        g.ok = true;
        g.open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(bw, 0)) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive())) {
        g.open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(bw, 0))) {
        bQuitProgram = true;
        g.open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Settings file: %s", file_name(sConfigFileName).c_str());
    ImGui::SetItemTooltip("%s", sConfigFileName);
    ImGui::End();
}

// The Media menu: disks changed at once, as in the Storage settings.
std::vector<MenuEntry> media_menu() {
    using E = MenuEntry;
    std::vector<E> m;
    for (int n = 0; n < MO_MAX_DRIVES; n++) {
        const MODISK& d = ConfigureParams.MO.drive[n];
        if (!d.bDriveConnected) continue;
        m.push_back(E::heading("MO drive " + std::to_string(n)));
        m.push_back(E::item(d.bDiskInserted ? file_name(d.szImageName) : "(no disk)", nullptr, false));
        m.push_back(E::item("Insert Disk...", [n] { insert_mo(n); }, !d.bDiskInserted));
        m.push_back(E::item("Eject", [n] { eject_mo(n); }, d.bDiskInserted));
        m.push_back(E::item("Read-only", [n] { ConfigureParams.MO.drive[n].bWriteProtected ^= true; }, true,
                            d.bWriteProtected)
                        .tip("For the next disk inserted"));
        m.push_back(E::separator());
    }
    {
        const FLPDISK& d = ConfigureParams.Floppy.drive[0];
        if (d.bDriveConnected) {
            m.push_back(E::heading("Floppy drive"));
            m.push_back(E::item(d.bDiskInserted ? file_name(d.szImageName) : "(no disk)", nullptr, false));
            m.push_back(E::item("Insert Disk...", [] { insert_floppy(0); }, !d.bDiskInserted));
            m.push_back(E::item("Eject", [] { eject_floppy(0); }, d.bDiskInserted));
            m.push_back(E::item("Read-only", [] { ConfigureParams.Floppy.drive[0].bWriteProtected ^= true; }, true,
                                d.bWriteProtected)
                            .tip("For the next disk inserted"));
            m.push_back(E::separator());
        }
    }
    for (int i = 0; i < ESP_MAX_DEVS; i++) {
        const SCSIDISK& t = ConfigureParams.SCSI.target[i];
        if (t.nDeviceType == SD_NONE) continue;
        const char* kind = t.nDeviceType == SD_CD ? "CD-ROM" : t.nDeviceType == SD_FLOPPY ? "Floppy" : "Hard disk";
        m.push_back(E::heading(std::string(kind) + " (SCSI " + std::to_string(i) + ")"));
        m.push_back(E::item(t.bDiskInserted ? file_name(t.szImageName) : "(empty)", nullptr, false));
        if (t.nDeviceType == SD_HARDDISK) {
            m.push_back(E::item("Change in Settings...", [] { ShortCut_Run(SHORTCUT_OPTIONS); })
                            .tip("A hard disk changes with a reset"));
        } else {
            m.push_back(E::item(t.nDeviceType == SD_CD ? "Insert Disc..." : "Insert Disk...", [i] { insert_scsi(i); },
                                !t.bDiskInserted));
            m.push_back(E::item("Eject", [i] { eject_scsi(i); }, t.bDiskInserted));
        }
        m.push_back(E::separator());
    }
    while (!m.empty() && m.back().kind == E::Kind::Separator) m.pop_back();
    if (m.empty()) m.push_back(E::item("No drives (see Settings > Storage)", nullptr, false));
    return m;
}

void open_settings_file() {
    const std::string path = choose_now(Pick::Open, sConfigFileName, kConfigFilters, Place::Base);
    if (path.empty()) return;
    const bool was_active = Main_PauseEmulation(true);
    CNF_PARAMS current = ConfigureParams;
    char old_name[FILENAME_MAX];
    SDL_strlcpy(old_name, sConfigFileName, sizeof old_name);
    copy(sConfigFileName, path);
    Configuration_Load(nullptr);
    bool ok = true;
    if (Change_DoNeedReset(&current, &ConfigureParams))
        ok = DlgAlert_Query("The emulated system must be reset to apply these changes. Apply changes now and reset "
                            "the emulator?");
    if (ok) {
        Change_CopyChangedParamsToConfiguration(&current, &ConfigureParams, false);
    } else {
        ConfigureParams = current;
        SDL_strlcpy(sConfigFileName, old_name, sizeof sConfigFileName);
    }
    if (was_active) Main_UnPauseEmulation();
}

void save_settings_file() {
    save_config();
}

}  // namespace ptgui

using namespace ptgui;

extern "C" {

// The Settings window, open until OK, Cancel or Quit (see Dialog_DoProperty).
int Dialog_MainDlg(bool* bReset, bool* bLoadedSnapshot) {
    *bLoadedSnapshot = false;
    g = Session{};
    g.open = true;
    {
        int banks[4];
        std::memcpy(banks, ConfigureParams.Memory.nMemoryBankSize, sizeof banks);
        Configuration_CheckMemory(banks);
        g.mem_custom = !is_known_layout(banks) ||
                       std::memcmp(banks, ConfigureParams.Memory.nMemoryBankSize, sizeof banks) != 0;
    }
    run_modal([] { return g.open; });
    st().event_hook = nullptr;
    g.open = false;
    *bReset = g.force_reset;
    if (!g.ok) return false;

    // What the original dialogs did on leaving them
    CNF_SYSTEM& sys = ConfigureParams.System;
    sys.bCompatibleCpu = true;
    sys.bCompatibleFPU = true;
    sys.bMMU = true;
    Configuration_CheckPeripheralSettings();
    for (int i = 0; i < ESP_MAX_DEVS; i++) {
        SCSIDISK& t = ConfigureParams.SCSI.target[i];
        if (t.nDeviceType == SD_NONE) {
            t.bDiskInserted = false;
            t.szImageName[0] = '\0';
            t.bWriteProtected = false;
        } else if (t.nDeviceType == SD_HARDDISK && (!t.bDiskInserted || !t.szImageName[0])) {
            DlgAlert_Notice("No image selected for harddisk drive. Ignoring drive.");
            t.nDeviceType = SD_NONE;
            t.bDiskInserted = false;
            t.bWriteProtected = false;
        } else if (t.nDeviceType == SD_CD) {
            t.bWriteProtected = true;  // CDs are read-only
        }
    }
    if (!ConfigureParams.Ethernet.bEthernetConnected) ConfigureParams.Ethernet.bTwistedPair = false;
    return true;
}

void Dialog_GraphicsCheckConsole(int slot) {
    const CNF_ND& nd = ConfigureParams.Dimension;
    if (nd.nConsoleSlot == slot && slot > 0 && nd.board[ND_NUM(slot)].bEnabled) {
        int banks[4];
        std::memcpy(banks, nd.board[ND_NUM(slot)].nMemoryBankSize, sizeof banks);
        if (Configuration_CheckDimensionMemory(banks) > 32)
            DlgAlert_Notice("NOTE: Console on NeXTdimension may not show up if NeXTdimension has more than 32 MB of "
                            "memory.");
    }
}

}  // extern "C"
