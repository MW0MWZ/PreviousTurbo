# PreviousTurbo

PreviousTurbo is a fork of [Previous](https://sourceforge.net/projects/previous/),
the NeXT computer emulator, with one aim: to make it much faster.

Previous is a careful, accurate emulator of the NeXT range. It runs the original
NeXT ROMs and NeXTSTEP unmodified, on the 68030 NeXT Computer and all the 68040
Cubes and Stations, including the Turbo and Color models and the NeXTdimension
board. PreviousTurbo keeps all of that and works on speed: how fast the guest
runs, how quickly it boots, and how responsive it feels on the desktop.

The name is a nod to both Previous and the NeXTstation Turbo. All the credit for
the emulator itself belongs to the Previous authors.

## Status

PreviousTurbo is at its very beginning. It is based on Previous 4.5 (SVN trunk
revision 1858). So far:

- **A new window.** Menus (Machine, Media, View, Help), a status bar with the
  drive, network, DSP and NeXTdimension lights and the speed (how many times
  as fast as the real CPU it runs when busy), and a tabbed Settings window
  replace the original full-screen dialogs. Every option of the original
  dialogs is kept. On macOS the menus are in the system menu bar.
- **Disks while running.** The Media menu inserts and ejects magneto-optical,
  floppy and SCSI CD-ROM disks without opening the settings.
- **Turbo.** The Turbo button in the status bar (also Machine > Turbo and
  Settings > Machine) runs the CPU as fast as the host can go, with fast disk
  transfers and a page cache in front of the 68040 MMU, while the machine's
  clocks keep to real time, as in SiliconGhost: the guest's time passes as
  the host's does, so its clock stays right and its own waits take as long
  as on the real machine. Programs run at the host's full speed; the
  kernel's code at half of it, and at most 12 times the real CPU's, so that
  the time it counts never falls behind (OPENSTEP reads its microsecond
  counter on the assumption that it does not jump). With Turbo off the
  emulation is that of Previous 4.5.
- **JIT.** With Turbo, the 68040's code is compiled for the host's processor
  as it runs, on x86-64 (Linux, macOS, Windows) and AArch64 (macOS, Linux),
  with the MMU, faults and self-modifying code handled as before; anything it
  does not compile runs as in the interpreter. It can be switched off in
  Settings > Machine, next to Turbo.
- **Clock.** The real-time clock keeps the host's time, as on battery: it is
  set from the host's clock at power on and moves only when the guest sets
  it. OPENSTEP 4.2 takes the date of its disk instead when the clock differs
  from it by more than 90 days ("preposterous time in Real Time Clock"), as
  after a long time unused; set the date in OPENSTEP once, and from the next
  start it follows the host's clock.
- **Cheaper screen updates.** Only the parts of the screen that changed are
  converted and drawn, and nothing is drawn when nothing changed.

## Plans

- **CPU.** More of the 68040's instructions compiled by the JIT, and a
  faster FPU path.
- **NeXTdimension.** A much faster i860.
- **Sound.** Steadier audio at high speeds.

Anything that departs from the behaviour of real hardware for the sake of speed
will be an option, so the accurate behaviour of Previous stays available.

## Using it

Press **F12**, or choose **Machine > Settings**, to set up the machine: its
model, memory, ROMs, disks, network and so on. The machine pauses while the
Settings window is open; **OK** applies the changes (asking first if the machine
has to be reset) and saves them, **Cancel** leaves everything as it was.

PreviousTurbo keeps its files in **PreviousTurbo** in your Documents folder
(`~/Documents/PreviousTurbo` on macOS and Linux, or the documents folder XDG
names; `Documents\PreviousTurbo` on Windows, wherever Documents has been
moved), made on first start. It holds the settings, **settings.cfg**, and
the folders **diskImages**, for disk, magneto-optical, floppy and CD-ROM
images, and **roms**, for ROMs of your own; the file choosers start in the
matching folder. The first time, PreviousTurbo starts from Previous's
settings if you have them. The NeXT ROMs come with PreviousTurbo and stay
with the program. CD-ROM images can
be plain (.iso) or raw (the .bin of a .bin/.cue pair).

Click the emulated screen to lock the mouse into the window; **Ctrl+Alt+G**
(Ctrl+Option+G on a Mac) releases it, as does switching to another window. Shortcuts are **Ctrl+Alt** and a letter (Ctrl+Option on a Mac),
shown in the menus; **Help > Keyboard and Mouse** lists them, and
**Settings > Keyboard** changes them.

See [readme.previous.txt](readme.previous.txt) for the machines Previous
supports, ROMs, disk images, networking and more.

## Building

You need:

- **CMake** 3.12 or later, **Ninja** (or make) and **pkg-config**;
- a **C compiler and a C++17 compiler**: GCC or Clang;
- **SDL3** for the window, keyboard, mouse and sound;
- optionally **libpng** (printing to PNG files), **libpcap** (networking
  without NAT) and **readline** (the debugger's command line), used when found.

Dear ImGui, used for the menus and dialogs, is included in `third_party/imgui`.
The NeXT ROMs are included too, in `src/`, and are copied beside the program
when it is built.

On **Debian 13** or **Ubuntu 25.04 and later**:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl3-dev libpng-dev zlib1g-dev libpcap-dev libreadline-dev
```

On **Fedora**:

```sh
sudo dnf install gcc gcc-c++ cmake ninja-build pkgconf-pkg-config \
    SDL3-devel libpng-devel zlib-devel libpcap-devel readline-devel
```

On **macOS** (Apple silicon or Intel), with the Xcode command-line tools for
the compiler and [Homebrew](https://brew.sh) for the rest:

```sh
xcode-select --install
brew install cmake ninja pkgconf sdl3 libpng
```

On macOS the build makes `build/PreviousTurbo.app`, a normal application with
the menus in the system menu bar (`build/previousturbo` links to the program
inside it, for running from the terminal). To make a copy that runs on its
own, with the libraries it uses copied in and signed for this Mac, in the
Applications folder:

```sh
cmake --install build --prefix /Applications
```

No `sudo` is needed: an administrator account can write to /Applications.
(The `install_name_tool` warnings about invalidated signatures are expected:
the copy is signed again as the last step.)

On **Windows**, with [MSYS2](https://www.msys2.org): in its UCRT64 shell,

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-libpng
```

`build/previousturbo.exe` then runs from that shell; to run it elsewhere,
copy the DLLs it uses from `/ucrt64/bin` next to it:

```sh
ldd build/previousturbo.exe | awk '/\/ucrt64\/bin\// {print $3}' | xargs -I{} cp {} build/
```

For networking without NAT on Windows, install [Npcap](https://npcap.com/) and
its SDK, and build with libpcap found.

Ubuntu 24.04 LTS and older releases do not package SDL3; build and install
it from source first ([SDL's build instructions](https://github.com/libsdl-org/SDL/blob/main/docs/README-cmake.md)).
Distribution SDL3 packages support both Wayland and X11 desktops; an SDL3
built from source needs the Wayland and X11 development headers installed
beforehand to support them.

Then, on every system:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build      # unit tests
./build/previousturbo
```

Check the configure output: it lists which optional libraries were found.
Clang works as well as GCC (`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`).
On Linux, `cmake --install build` installs the program and its ROMs (under
`/usr/local` unless `CMAKE_INSTALL_PREFIX` says otherwise).

A macOS build is for the Mac it is built on. One application for both Apple
silicon and Intel Macs needs universal builds of SDL3 and libpng (Homebrew's
are for one kind of processor), such as `SDL3.framework` from
[libsdl.org](https://www.libsdl.org/), and
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0`.

### Build options

Set with `-D<option>=ON` or `OFF` when running `cmake -S . -B build`:

| Option | Default | |
|---|---|---|
| `ENABLE_DSP_EMU` | ON | Emulate the DSP56001 |
| `ENABLE_TRACING` | ON | Tracing messages for debugging |
| `ENABLE_ASAN`, `ENABLE_UBSAN` | OFF | Address and undefined-behaviour sanitizers |
| `ENABLE_OSX_BUNDLE` | ON on macOS | Build `PreviousTurbo.app` |
| `PT_BUILD_TESTS` | ON | Build the unit tests (`ctest --test-dir build`) |

The original build notes are in [building.previous.txt](building.previous.txt).

## Credits

Previous is written by Andreas Grabher, Simon Schubiger and Gilles Fétis, and is
based on the Atari ST emulator [Hatari](https://www.hatari-emu.org/). It uses the
m68k core from WinUAE and Jason Eckhardt's i860 emulator. The full list of
contributors is in [readme.previous.txt](readme.previous.txt).

The menus and dialogs use [Dear ImGui](https://github.com/ocornut/imgui) by
Omar Cornut (MIT licence, see `third_party/imgui/LICENSE.txt`).

## Licence

PreviousTurbo, like Previous, is free software under the GNU General Public
License, version 2 or (at your option) any later version. See
[gpl.txt](gpl.txt).
