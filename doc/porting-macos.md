# Porting to macOS

The project was written for MSYS2 / MinGW-w64 on Windows. This documents the
macOS port: what works today, how the platform split is laid out, and what is
left. It is a port, not a rewrite — the emulator core is untouched apart from
one compiler fix.

Target: **Apple silicon (arm64) only.** Build with the system clang++.

## Status

| Step | What | State |
|---|---|---|
| 1 | Core emulator + offline tools (`verify`, `boot`, `render`, `panel`, `statetest`) | **done** |
| 2 | Real-time audio (CoreAudio) + MIDI in/out (CoreMIDI) for `live` | **done** |
| 3 | GUI window (`gui`) — CoreGraphics drawing + Cocoa window | **done** |
| 4 | VST3 bundle for `Contents/MacOS` + `probe` | **done** |
| 5 | Audio Unit wrapper (AUv2, `aumu`) + `au-probe` | **done** |

```
make          build every tool and both plug-in bundles
make check    ROM-free sanity check (runs build/verify)
make probe    load the VST3 bundle in a headless host
make au-probe load the AU bundle in a headless host
make check-au same, plus the AU's torture test
```

## The core needed one change

`timer_alloc` in `src/compat/mamecompat.h` called `machine().make_timer(...)`
while `running_machine` was still an incomplete type. GCC accepts that inside a
member template and defers the check to instantiation; **Apple Clang rejects it
at definition time**. The definition was moved below `running_machine`, the same
way MAME splits declaration from definition.

Two other spots were x86-specific and were made portable:

* `_mm_pause()` (x86 spin hint) → `smu2000::cpu_pause()` in
  `src/compat/platform.h`. It emits `PAUSE` on x86 and `YIELD` on arm64.
* `SetConsoleOutputCP(CP_UTF8)` → `smu2000::init_console_utf8()` in
  `src/compat/console.h`. A no-op on macOS, where terminals are UTF-8 already.

`src/render.cpp` and `src/statetest.cpp` only carried `#include <windows.h>`
without using anything from it; the include was dropped.

### Numeric fidelity

The audio is compared against MAME recordings, so the math has to be
reproducible. Apple Clang on arm64 may contract `a*b+c` into a fused
multiply-add, which baseline x86-64 (no FMA) cannot — so the port was checked
for it. Compiling the whole DSP path to assembly shows **zero fused operations**,
so the numbers match the Windows build without needing `-ffp-contract=off`.

State serialization (`src/state.h`) writes fixed-width types only, so the
platform differences in `long` do not reach the saved bytes.

## Platform layout

Shared logic stays in one place; only the OS edge is split.

| Concern | Windows | macOS |
|---|---|---|
| Audio output | `src/ui/audio_out.cpp` (WASAPI) | `src/ui/audio_out_mac.cpp` (CoreAudio) |
| MIDI input | `src/ui/midi_in.cpp` (WinMM) | `src/ui/midi_in_mac.cpp` (CoreMIDI) |
| MIDI output | `src/ui/midi_out.cpp` (WinMM) | `src/ui/midi_out_mac.cpp` (CoreMIDI) |
| GDI subset | `compat/gdi.h` → `<windows.h>` | `compat/gdi.h` + `compat/gdi_mac.cpp` (CoreGraphics) |
| Panel / editor / effects drawing | `src/ui/{panel,editor,effects,layout,svg}.cpp` — **the same files** | ditto |
| Window, events, menus | `src/gui.cpp` (Win32) | `src/gui_mac.cpp` + `src/ui/window_mac.mm` (AppKit) |
| Real-time MIDI file playback | `src/ui/player.cpp` (Win32 timers) | `src/ui/player.cpp` (`std::chrono`) |
| Settings and file lookup | `src/compat/paths.h` (`%LOCALAPPDATA%`) | `src/compat/paths.h` (`~/Library/Application Support`) |
| VST3 | `src/vst3/*` (`x86_64-win`) | `src/vst3/view_mac.mm` + `Contents/MacOS` |
| Audio Unit | — | `src/au/plugin.cpp` (`aumu`) |

`src/ui/audio_out.h` and `src/ui/midi_in.h` hold **both** class definitions under
`#if defined(__APPLE__)`. The public interface is identical on each side, so
`live` and the GUI are unaware of which implementation they get. The Windows
bodies are kept verbatim in the `#else` branch.

The macOS `audio_out` is a pimpl. Its constructor and destructor are declared in
the header and defined in the `.cpp`: with a `unique_ptr` member that is not
optional, because the compiler otherwise generates them where `impl` is still
incomplete.

### Comment language

The two conventions coexist, and the split is by **author, not by file**:

- Comments that came across from the existing Windows code **keep their original
  Japanese**. They are the author's prose; moving a block into `src/ui/engine.h`
  or `src/vst3/view_win.cpp` does not change who wrote it, so it is not
  retranslated. Searching a comment you remember from `gui.cpp` may now find it in
  one of those extracted files, unchanged.
- Comments the port adds are **English**: the whole of every new file, and any
  line written to explain a macOS path or a platform difference. Where a port note
  belongs beside an existing block the English line goes *below* the Japanese one
  rather than replacing it — `src/vst3/engine.cpp` and `src/ui/midi_in.h` show the
  shape.

User-facing strings are a separate question and were left alone: the panel, the
console messages and the error texts are still Japanese, so both platforms show
and print exactly the same thing. `--list`, the status line and the port menus on
macOS read identically to their Windows counterparts.

## How the audio port works

The central rule from [design.md](design.md) — **the synth owns no clock** — maps
onto CoreAudio almost directly. The Windows side runs a worker thread around
WASAPI; on macOS a `DefaultOutput` AudioUnit calls our render callback on its own
real-time HAL thread, so there is no thread to manage at all.

* Client format is 44100 Hz, 16-bit, stereo, interleaved — exactly what the
  synth generates. The unit converts for the device.
* `--latency` sets `kAudioDevicePropertyBufferFrameSize` on the default output
  device (clamped to the device's range). Not setting it leaves CoreAudio's
  default, often 512 frames.
* Real-time priority comes for free: the HAL thread is already real-time, so
  `mmcss()` reports whether the unit started.
* WASAPI exposes "frames still queued", which makes an underrun directly
  observable. CoreAudio does not, so `starved()` uses the same proxy `live`
  already tracked: a fill that took longer than the block it was making means
  the device would have run dry.

### MIDI input

CoreMIDI hands over whole packets, SysEx included, on its own thread. That
removes the Windows trap where SysEx silently disappears unless you pre-post
receive buffers to WinMM. The lock-free ring between the callback and the audio
thread is unchanged, because the audio thread still drains it a byte at a time.

`list()` and `open(i)` both walk `MIDIGetSource(i)` in order, so the index a
user passes to `--midi` matches the listing.

### MIDI output

The THRU path — anything the synth receives is echoed to MIDI OUT — is
`src/ui/midi_out_mac.cpp`. It keeps the same split as the Windows side, and it
matters more here: the audio thread has to hand a byte over without ever
blocking, and `MIDISend` can take a lock, so calling it from the render callback
would put an unpredictable wait inside the audio callback. The audio thread only
drops bytes into a ring; second thread sends.

The wake-up differs, because CoreMIDI has no event object to signal. A
`std::condition_variable` stands in for the Win32 event.

One detail worth knowing if the SysEx path is ever touched: CoreMIDI wants the
packet list in a buffer the caller owns, and a dump can be tens of kilobytes, so
the buffer is sized for the whole message and `MIDIPacketListAdd` lays it out.
The WinMM version instead prepares a header and waits for `MHDR_DONE`.

`list()` and `open(i)` walk `MIDIGetDestination(i)` in order.

## How the GUI port works

This is the part that could have been a rewrite, and deliberately is not.

### One drawing layer, two implementations

Every pixel of the panel, the editor page, the effects page and the SVG art goes
through about twenty GDI calls. Those are re-declared in `src/compat/gdi.h` —
which is nothing but `#include <windows.h>` on Windows — and implemented once
over CoreGraphics in `src/compat/gdi_mac.cpp`.

`draw.h`, `panel.h`, `layout.h` and `svg.h` include `compat/gdi.h` instead of
`<windows.h>`, and that is the **entire** change to the drawing code: four
include lines, no edits to `panel.cpp`, `editor.cpp`, `effects.cpp`,
`layout.cpp` or `svg.cpp`. A hand-ported drawing layer has to be compared by eye
to know it matches. With the surface pinned, the two panels agree by
construction.

What the shim reproduces, and why each matters:

* **No flipping.** Every context it hands out has its origin top-left with y
  running down, like GDI. A bitmap context starts bottom-left, so it is turned
  over once when it is made; a context from a flipped `NSView` already is that
  way and is left alone.
* **The half-pixel nudge.** GDI paints a 1-pixel line on exact pixel
  boundaries: a rule at y = 5 covers row 5. CoreGraphics centres the line on
y = 5 and would straddle rows 4 and 5. Odd pen widths are shifted by half a
  pixel, which is what keeps the LCD's tick marks and the editing grid crisp.
  Even widths already line up and are left alone.
* **Rectangles.** `[left, right) × [top, bottom)`, GDI's convention.
* **`ALTERNATE` fill rule** (even-odd), because the SVG art has holes in it.
* **`Arc`** is flattened to a polyline. It is used once, for the send-level
  fans, and the context here is y-down; sampling the angles is unambiguous where
  untangling CoreGraphics' flipped angle signs is not.
* **A default pen and brush.** A fresh DC starts with `BLACK_PEN` and
  `WHITE_BRUSH`, exactly as GDI's does, so a `Polygon` with no pen selected
  still gets its outline.

Text is the one place that cannot match exactly. `CreateFontA` is asked for
"Segoe UI", which macOS does not have, so that request is answered with the
system UI font. Glyph metrics differ slightly from the Windows build, which
means layout tuned around text via `panel.txt` may want a nudge. Everything else
— colours, geometry, line weight — is identical.

### Why Cocoa lives in a separate file

Cocoa's headers define `BOOL` and Quickdraw's (pulled in through AppKit) define
`Polygon`. `compat/gdi.h` has to declare both to keep `panel.cpp` unchanged, so
the two cannot be in one translation unit. `src/ui/window_mac.mm` is therefore
the only Objective-C++ file in the project and never includes `gdi.h`; it talks
to the app through the plain-C++ `ui::mac_app` interface in
`src/ui/window_mac.h`. That split is also just tidier: one file knows about
windows, menus and file panels, and knows nothing about a synthesizer.

The window is an `NSWindow` with a flipped `NSView`. Painting goes straight into
the view's `CGContext` wrapped by `smu_gdi_wrap_view_context()`; AppKit already
double-buffers, so `gui.cpp`'s memory DC has no counterpart. A 33 ms timer drives
repaints, in common run loop modes so it keeps ticking while a menu is open.

The port picker is an `NSMenu` built from a plain description the app returns,
and choosing a MIDI file is an `NSOpenPanel`; the app asks for the latter by
name (`ui::open_midi_file_panel()`), so it never needs AppKit itself.

### Sharing the engine rather than copying it

The synth half of `gui.cpp` — load the ROMs, boot, and render blocks — moved to
`src/ui/engine.h` and both front ends use it. The point is not the boilerplate
but the routing in `fill()`: which port feeds which part, and what gets echoed
back out. Two copies of that are two things that can drift, and a drift there
would show up as the platforms sounding different. `gui.cpp` now pulls the name
in with a `using` declaration, so the rest of it reads as it always did.

Three smaller things became portable instead of being duplicated:
`src/compat/paths.h` (the executable directory and the settings directory),
`src/ui/player.cpp` (a `std::chrono` clock in place of
`QueryPerformanceCounter` and `Sleep`; `steady_clock` is QueryPerformanceCounter
underneath on Windows, and `timeBeginPeriod(1)` is kept behind `_WIN32`),
and `src/ui/png.cpp`, which turned out to be hand-rolled zlib and never needed
GDI+ at all.

## How the VST3 port works

### The bundle

A macOS VST3 is `S-MU2000.vst3/Contents/MacOS/S-MU2000` plus
`Contents/Info.plist` (`packaging/vst3-macos-Info.plist`) and `Contents/PkgInfo`.
The `Info.plist` is not decoration: nothing `dlopen`s a `.vst3` directory. The
host opens it with `CFBundle`, reads `CFBundleExecutable` to find the binary, and
calls `bundleEntry`. So the entry points in `src/vst3/plugin.cpp` are per
platform — `InitDll`/`ExitDll` on Windows, `bundleEntry`/`bundleExit` on macOS —
and both hand back the same factory.

It is linked with `-bundle`, not `-shared`, because that is what `CFBundle`
loads.

### The view

`view.cpp` used to own an `HWND`. It is now split the same way the GUI is: the
VST3 interface, the panel and the input semantics stay in `view.cpp`, and the
window itself moves behind `src/vst3/plug_window.h` — `view_win.cpp` for the
child `HWND`, `view_mac.mm` for an `NSView` added to whatever view the host hands
over in `attached()`.

`view.h` mentions no window system at all and holds the panel behind a pimpl, so
`view_mac.mm` can include it next to Cocoa. That is the same `BOOL`/`Polygon`
collision that forced `window_mac.mm` apart from `compat/gdi.h`, and it is also
why `view.cpp` can include `gdi.h` and CoreGraphics together: it is Cocoa, not
CoreGraphics, that clashes.

The subview is flipped, so the context AppKit hands to `drawRect` is already
top-left with y down and goes straight into `smu_gdi_wrap_view_context()`. No
backing store either, unlike the Windows side: AppKit double-buffers already.
There is no `WM_TIMER` to drive repaints, so the view runs its own 33 ms timer in
common run loop modes. Key codes are mapped to `plug_key` in the `.mm`
(honouring the same characters `gui_mac.cpp` does), and `view.cpp` maps
`plug_key` to `mu2000::button` in one place, so the two platforms cannot
disagree about what a key does.

The probe's `--view` mode needed the same treatment: `probe.h`'s Win32 host
window became `src/vst3/probe_host.h`, implemented by `probe_host_win.cpp` and
`probe_host_mac.mm`. The macOS one runs the real `[NSApp run]` loop, because the
plugin's repaint timer lives on the run loop and its editor does not animate
otherwise.

### The probe

`vst3probe` opens the module the way each platform does — `LoadLibrary` on
Windows, `CFBundle` + `bundleEntry` on macOS — and the timing calls
(`GetTickCount`, `Sleep`) became `std::chrono` and `std::this_thread`.

## How the Audio Unit port works

The AU reuses the VST3 engine unchanged. `src/au/plugin.cpp` is only the host
interface; `midi()`, `fill()`, the resampler, the ROM search, the boot thread
and the state packer are all the same code the VST3 plug-in runs.

An Audio Unit is registered rather than opened: `Info.plist`'s `AudioComponents`
array (`packaging/au-macos-Info.plist`) names the type (`aumu`, a MusicDevice),
the subtype (`SMU2`), the manufacturer (`Trbh`) and the `factoryFunction`
(`SMU2000AUFactory`). A mismatch between that file and the constants in
`plugin.cpp` means either the AU is invisible to hosts or it loads and offers
nothing. Apple's `AudioUnitSDK` is not vendored — for the same reason Steinberg's
`public.sdk` is not: the dispatch is a fixed table, so it is written out here
instead of pulling in a framework.

The pieces worth knowing about:

* **`AudioComponentPlugInInterface`** with `Open`/`Close`/`Lookup`. The object's
  interface must be its first member, because the host is handed `&iface` and
  hands the same pointer back as `self` for every method.
* **MIDI scheduling.** `MusicDeviceMIDIEvent` can be called from a thread that is
  not the audio thread, and the `engine`'s MIDI entry is audio-thread-only (it
  touches the pre-boot queue), so events are parked in a queue behind a mutex
  and drained inside `Render` with `try_lock` — if the lock is busy, the
  messages are picked up in the next block. The engine's own MIDI path is never
  called from anywhere but the render thread.
* **Offsets are honoured the same way the VST3 side does it**: `fill()` runs up to
  each event's offset, the event is injected, then `fill()` continues. That is
  what makes the AU and VST3 renders line up sample-for-sample rather than
  block-for-block.
* **Interleaved output** (one buffer holding two channels) is de-interleaved
  256 frames at a time into scratch buffers; the non-interleaved case, which is
  what a host normally asks for, writes straight into the host's buffers.
* **`ClassInfo`** carries the same `state_pack()` blob the VST3 plug-in stores,
  so the AU's state is ~330 KB rather than the 6 MB raw machine image. The
  output level rides alongside it as a `CFNumber`.
* **A restore that arrives before boot is parked, not dropped.** A host sets
  `ClassInfo` straight after `AudioComponentInstanceNew` — exactly when the boot
  thread is still running — and the machine cannot be written back until it has
  come up. `engine::load_state()` used to refuse while the state was `loading`,
  **in silence**, so the restore vanished and the unit came up at its defaults.
  (The output level still landed, because that is a plain member and not part of
  the machine — which is what made it look like it had worked.) Now the buffer
  is parked and `boot()` applies it at the end of boot, before it publishes
  `ready`, so a host that reads straight back does not see the old machine.
  `vst3/plugin.cpp`'s `setState` no longer needs its wait either.

  A timeout was the wrong shape twice over: boot measured **3.6 s** of wall
  clock on a cold instance here, so the 3 s wait the VST3 used lost the restore,
  and simply waiting longer would block the host instead. Parking has no timing
  assumption in it.

  It was pinned down with a scratch host (not in the tree yet): one instance
  boots and is then given a program change so its state is distinctive, another
  is handed that state *immediately* after `AudioComponentInstanceNew`, and the
  raw (unpacked) state bytes are compared. Matching the first exactly means the
  restore survived; matching the default boot state means it was dropped. Two
  plain boots are compared as well, so the boot state is known to be
  reproducible and the verdict cannot be an artefact of one run.

  The failure was intermittent, which is what gave the timeout away: with a 3 s
  wait it read "restored" on a warm instance and "DROPPED" on a cold one. The
  same scratch host is worth keeping — `aubprobe --torture` cannot see this,
  because it waits for the blob to grow before it sets anything.
* **Latency** is reported from `latency_samples()`, which is non-zero only when
  the host's rate is not 44100 (then the resampler adds the delay).
* **Parameters** are two, not the VST3 side's 2098. AU has no MIDI-CC-to-parameter
  convention like VST3's `IMidiMapping`, so there is nothing to map; MIDI arrives
  through the MusicDevice entry points instead, and the two parameters are what a
  host's generic panel can usefully show (output level, and whether the firmware
  has come up).

`make au` writes `build/S-MU2000.component`; `make install-au` copies it to
`~/Library/Audio/Plug-Ins/Components`, which is where `auval` and every DAW look.

## Building

```
make   verify / boot / render / panel / statetest / live / gui
       and both bundles: S-MU2000.vst3 and S-MU2000.component
```

The Makefile detects the platform: `OS=Windows_NT` → Windows, `uname -s` =
`Darwin` → macOS. The Windows recipes keep the same target names, flags and
libraries as before. The two Objective-C++ files get their own pattern rule,
because they have to be compiled with `-fobjc-arc`.

## Verifying

Without ROMs only the ROM-free checks can run:

```
make check
```

which exercises the SWP30 register file and the machine's random sequence.

CoreAudio can be checked on its own, without ROMs:

```
clang++ -std=c++20 -O2 -I src -I src/compat -x c++ -c -o /tmp/audiotest.o - <<'EOF'
#include "ui/audio_out.h"
...
EOF
clang++ /tmp/audiotest.o build/src/ui/audio_out_mac.o \
    -framework CoreAudio -framework AudioToolbox -framework AudioUnit \
    -framework CoreFoundation -o /tmp/audiotest
/tmp/audiotest
```

On this machine that reports `buffer_frames=1323` for `--latency 30` and pulls
about 13371 frames in 300 ms (expected ~13230), `starved=0`.

**Verifying the sound itself needs ROMs.** Dump them as described in
[doc/dump/](dump/) and point `live` at the directory. `live --list` shows MIDI
inputs (`MIDI 入力が見つからない` when there are none).

### Checking the drawing layer

`--shot` renders a page to a PNG with no window at all, which is the quickest
way to see the panel:

```
./build/gui --shot /tmp/panel.png --size 1400x360          # empty panel
./build/gui --shot /tmp/grid.png  --size 1400x360 --grid   # with the editing grid
./build/gui roms --boot --shot /tmp/boot.png               # with the firmware up
./build/gui --layout art/mame/panel.txt   --shot /tmp/art.png --size 1000x420
./build/gui --layout art/sample/panel.txt --shot /tmp/art.png --size 1000x420
```

The last two are worth running because the SVG art is the only thing that
exercises `PolyPolygon`. `art/mame/panel.txt` draws the whole MAME panel as one
SVG, and the art contains colours that are not in the built-in palette — so
finding `#404040` in the output, and *not* in a run without the art, proves the
SVG was parsed and filled rather than silently skipped.

There is also a direct check that the window path and the `--shot` path agree,
which is what makes the PNGs a valid stand-in for what the window shows: paint
the same panel twice, once into a DIB and once into a context that has already
been flipped the way AppKit flips it, and compare the bytes.

### What was checked, and what was not

Verified on arm64 against the working tree:

| Check | Result |
|---|---|
| `make` from clean | 9 executables + both bundles, no warnings |
| `make check` | random sequence `574a3af2 de214fbe 610c06da`, as before |
| `statetest` | packed state **332767** bytes, restore exact |
| `render` (Bhangra, XG) | **634.921** cycles/sample, 8583140 word writes, 0 byte writes — identical to before this step |
| `--shot` of the built-in panel | renders; palette matches (`#c4bdaa` face, `#d8cda5` keys) |
| `--shot` with `art/mame` and `art/sample` | SVG parsed and filled; art-only colour present |
| DIB path vs flipped view context | **0** differing bytes of 360000 |
| `gui` window | runs, CoreAudio real-time thread at 30 ms, stays up |
| Windows recipes | dry run unchanged (`-static`, `-lwinmm -lgdi32 …`) |

The render numbers matter most: they are the same as the pre-port build, so none
of this step disturbed the audio path.

**Not checked.** The window was confirmed to run and to paint identically to the
verified PNG path, but it was not looked at on screen, and the popup menus, the
file panel and keyboard/wheel input were not exercised interactively — that
needs someone at the machine. The editor and effects pages share every primitive
with the front page (they differ only in which controls they draw), but like the
front page they were only checked as PNGs, not clicked.

The plug-in view is in the same position: `vst3probe --view 6` reports
`createView` / `attached` / `removed` and stays up for six seconds with the panel
animating, the AU's probe drives the same window path, but neither plug-in's
window was looked at on screen.

## Plug-ins

### VST3

```
make vst3                     write build/S-MU2000.vst3
make probe                    load it in a headless host and describe it
make install-vst3             copy to ~/Library/Audio/Plug-Ins/VST3

build/vst3probe build/S-MU2000.vst3                       describe
build/vst3probe build/S-MU2000.vst3 song.mid out.wav      render
build/vst3probe build/S-MU2000.vst3 --torture             abuse it
build/vst3probe build/S-MU2000.vst3 --view 20             show the panel
```

The ROMs are found through `S_MU2000_ROMS`, or from `roms.txt` next to the
bundle, or from `~/Library/Application Support/S-MU2000/roms`.

Checked on arm64:

| Check | Result |
|---|---|
| `vst3probe` | factory found, 1 class, `aumu`-equivalent audio module, 2098 parameters, 2096 MIDI mappings |
| `--torture` | initialize/terminate cycles, 22050–192000 Hz, zero-length buffers, 4 simultaneous instances — **0 problems** |
| state save/restore | **330109** bytes, round-trips |
| 4 instances at once | 2.13 s of audio in 4.24 s wall (50% CPU each) |
| render vs `render` | 0 of 2194 windows where the reference plays and the plug-in does not (100% agreement) |

### Audio Unit

```
make au                       write build/S-MU2000.component
make aubprobe                 (via make au-probe) describe it
make install-au               copy to ~/Library/Audio/Plug-Ins/Components
make check-au                 the torture test

build/aubprobe build/S-MU2000.component --list                 describe
build/aubprobe build/S-MU2000.component song.mid out.wav       render
build/aubprobe build/S-MU2000.component --torture              abuse it
auval -v aumu SMU2 Trbh                                        Apple's own check
```

Both `make probe` and `make au-probe` pass the **bundle directory**, not the
executable: the probes open the bundle with `CFBundle`, so handing them
`Contents/MacOS/S-MU2000` stops them with "cannot open bundle". They also
export `S_MU2000_ROMS=$(ROMS)` (`ROMS ?= roms`), because neither plug-in can
boot without the ROMs.

`aubprobe <bundle>` reads the bundle with `CFBundle` and registers its factory
with `AudioComponentRegister`, so it needs nothing installed. Passing `-`
instead of a path uses whatever is registered for `aumu/SMU2/Trbh` on the
machine — which is the path `auval` takes, so the two can be compared.

Checked on arm64:

| Check | Result |
|---|---|
| `aubprobe --list` | opens; `Output Level` 0–1 and `Status` 0–2; instrument name; latency 0 |
| `--torture` | 8 create/dispose without initialize, 3 initialize cycles, 22050–192000 Hz, zero-length render, property table, state round-trip, 4 instances — **0 problems** |
| state save/restore | **329051** bytes (packed), round-trips |
| 4 instances at once | 2.13 s of audio in 4.20 s wall (49% CPU each) |
| render vs `render` | 0 of 2194 windows missing; envelope within ±10%, the same scatter the VST3 side shows |
| AU render vs VST3 render | aligned at shift 0.00 s, 0 windows missing |

The last line is the useful one: driven with identical MIDI at identical offsets,
the AU and the VST3 bundle produce the same music.

### `auval`

Apple's validator is stricter than either probe — it wants a specific set of
properties to answer, in specific scopes, with the right writability — and it
only sees components in the standard `Components` directories, so it needs
`make install-au` first. It now passes:

```
make install-au
auval -v aumu SMU2 Trbh
  ... AU VALIDATION SUCCEEDED.
```

Three things stood out while getting there, each worth knowing before changing
the AU again:

- **`sandboxSafe` has to be `false` in `Info.plist`.** With it set, `auval`
tries to open the component inside its own sandbox, and the ROM lookup — which
reads from a directory the user chose, not from the bundle — fails. The flag is
a promise the AU cannot keep here.
- **A missing property answer reads as a failure, not as "unimplemented".**  `auval` walks a fixed table of scopes and asks for each in turn; anything that
  comes back `kAudioUnitErr_InvalidProperty` is reported rather than skipped, so
  the wrapper has to answer the whole table.
- **The scope is part of the answer.** `kAudioUnitProperty_SupportedNumChannels`
  is documented as **Global**, and `DLSMusicDevice` — Apple's own `aumu` —
  refuses it for both Input and Output. `aubprobe` used to ask for it in Output,
  which accused a unit that answers exactly the way Apple's does. The probe now
  asks in Global, and additionally checks the refusals it and DLSMusicDevice
  share (`SupportedNumChannels`, `Latency` and `MaximumFramesPerSlice` for
  Output, `StreamFormat` for Input, `ClassInfo` for Output). When a scope
  question comes up, ask the reference: the pairs above were read off
  DLSMusicDevice rather than guessed.

### Where the ROMs are looked for

The ROMs are Yamaha firmware the user supplies, so they cannot ship inside the
bundle. `find_roms()` in `src/vst3/engine.cpp` tries each of these in order and
takes the first directory holding `mu2000_flash.bin`:

| | where |
|---|---|
| 1 | `$S_MU2000_ROMS` |
| 2 | the bundle's `Contents/Resources`, then `Contents/Resources/roms` |
| 3 | next to the binary, and `<binary dir>/roms` |
| 4 | a `roms.txt` naming a directory, next to the binary or in `Contents/Resources` |
| 5 | `~/Library/Application Support/S-MU2000/roms`, that directory itself, and a `roms.txt` in it |
| 6 | `~/Documents/S-MU2000/roms` |
| 7 | `/Library/Application Support/S-MU2000/roms`, that directory itself, and a `roms.txt` in it |

Steps 5-7 are the macOS places a plug-in's own data belongs: the per-user
Application Support directory, Documents, and the **machine-wide** Application
Support directory. The per-user answers come first on purpose, so a user's own
copy beats a shared install. Steps 6 and 7 exist for the AU in particular: it is
one bundle in `Components`, used by every account on the machine, and it has no
window in which to be told a path. Step 7 is never written to — creating it
takes an installer with the rights to.

`S_MU2000_ROMS` is still the strongest answer, which is what the Makefile uses
(`ROMS ?= roms`). When nothing is found, the message and the log list every path
tried, so it is visible which one was expected.

`gui` is the exception: it takes the ROM directory as an argument and does not
search these. It is a program you run from a checkout, with somewhere to type.

## A note on the three Windows-only tools

`midisend`, `rec` and the old `--waveout` path in `live` are development aids
for comparing against a real MU2000 over a Windows audio stack. They are not
built on macOS yet. `live` on macOS uses CoreAudio unconditionally.
