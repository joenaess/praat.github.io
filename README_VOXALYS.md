# Voxalys Native C++/Rust Extensions & JoVoice Core for Praat 7

This repository is a clean fork of **[Praat](https://www.fon.hum.uva.nl/praat/)** (Paul Boersma & David Weenink, University of Amsterdam) containing **only** native **Voxalys** clinical voice analysis, an interactive real-time **PortAudio Voice Range Profile (VRP)** editor with a live **piano keyboard**, and the pure-Rust **JoVoice** DSP engine compiled directly into Praat as a statically linked C-ABI library (`libvoxalys_rust.a`).

* **Author:** Jonas Lindh ([hejAI AB](https://www.hejai.se/))
* **Original paper:** Lindh, J. (2007), *"Voxalys – a Pedagogical Praat Plugin for Voice Analysis"*, Proceedings of Fonetik 2007, TMH-QPSR, Vol. 50(1), pp. 45–48.
* **Companion Mobile App:** **JoVoice** (Android real-time gamified vocal trainer) — info & demo at **[hejai.se](https://www.hejai.se/)**.
* **License:** **GNU General Public License v3.0 or later (`GPL-3.0-or-later`)**

---

## Where to Find It & How It Works in Praat

### 1. Live Interactive Voice Range Profile (`New` $\rightarrow$ `Voxalys` $\rightarrow$ `Record Interactive VRP (live)...`)
Opens the native `VoxalysVrpRecorder` window powered by PortAudio (`paFloat32` mono) and a lock-free Rust SPSC ring buffer (`VoxalysStreamHandle` — zero allocations or locks on the audio thread):

* **Logarithmic Semitone X-Axis + Aligned Piano Keyboard:** Defaults to a musical semitone scale (`50 Hz` floor out of the box, ideal for bass voices as well as sopranos) with an aligned white/black piano keyboard directly below the VRP scatter plot (`C2`–`C6` labels + Hz ticks).
* **Live Real-Time Biofeedback:**
  * Plots $(F_0, \text{SPL})$ points in real time with chronological color progression (`Blue -> Cyan -> Green -> Magenta -> Red`) and a live per-semitone upper/lower VRP envelope contour.
  * Highlights the actively sung piano key live (**cyan** for white keys, **red** for black keys) and displays live `F0 (Hz + Note)`, `SPL (dB)`, `Quality`, and `Phonation` class in the status bar.
* **Instant Visual Range Controls (without clearing recorded data):**
  * Toolbar buttons: **`Bass (50-450)`**, **`Full (50-1050)`**, **`Auto-Fit`**, and **`Scale: Log` / `Scale: Hz`**.
  * **`View` menu:** **`Display ranges...`**, **`Auto-fit to recorded points`**, presets (**Bass / Baritone**, **Tenor / Alto**, **Soprano**, **Full Clinical**), and scale toggle.
* **Direct Objects Window Integration:** Click **`Save Sound & VRP to List`** to publish both the uninterrupted recorded `Sound` and a `Table` (`time_s`, `f0_Hz`, `f0_st_re100Hz`, `spl_dB`, `voiced`, `quality`, `phonation`) directly into Praat's Objects list.

### 2. Native Acoustic & JoVoice Voice Report (`Sound` selected $\rightarrow$ `Voxalys -` $\rightarrow$ `Voxalys analyze voice & JoVoice metrics...`)
Combines Praat's native cross-correlation pitch (`Sound_to_Pitch_rawCc`), glottal pulse perturbation (`Sound_Pitch_to_PointProcess_cc`), harmonicity (`Sound_to_Harmonicity_cc`), and LTAS slope (`Sound_to_Ltas`) with Deliyski (1993) / MDVP clinical pathology threshold flags and the **JoVoice** Rust DSP summary (including semitone $\times$ dB VRP area and phonation quality score).

### 3. Batch Streaming VRP Table (`Sound` selected $\rightarrow$ `Voxalys -` $\rightarrow$ `Voxalys compute VRP trajectory Table...`)
Streams any selected `Sound` through the exact same lock-free Rust SPSC streaming VRP engine used by the live recorder, producing a Praat `Table` for reproducible clinical batch analysis and automated CI testing without a microphone.

---

## Architecture Overview

```text
external/voxalys_rust/          Pure-Rust static library (libvoxalys_rust.a)
├── Cargo.toml                  GPLv3-compatible deps only (ringbuf 0.4)
├── cbindgen.toml               C/C++ header generation configuration
├── Makefile                    Praat external/ build hook
├── include/voxalys_rust.h      Generated C/C++ FFI header
└── src/
    ├── lib.rs                  Crate root
    ├── ring_buffer.rs          Lock-free SPSC audio ring buffer (zero alloc/locks in callback)
    ├── vrp_analyzer.rs         Incremental sliding-window VRP processor (10 ms hop)
    ├── jovoice_bridge.rs       JoVoice YIN F0, RMS/SPL dB, phonation quality & VRP area
    └── ffi.rs                  Panic-safe extern "C" FFI with thread-local error reporting

dwtools/
├── Voxalys.h                   C++ declarations & VoxalysVrpRecorder (Editor subclass)
├── Voxalys.cpp                 Native Praat analysis, PortAudio VRP live recorder, piano strip & Table export
└── praat_Voxalys_init.cpp      Praat Objects menu & Sound action registration

test/dwtools/voxalys.praat      Headless Praat regression tests
.github/workflows/voxalys-ci.yml  GitHub Actions CI (Rust + Praat build & tests)
```

---

## Platform Support

| Platform | Status | How to Run / Build |
| :--- | :--- | :--- |
| **Linux (x86_64 / ARM64)** | **Supported & CI-verified** | Native build via `make -j"$(nproc)"` (GTK3 + PulseAudio/PipeWire/ALSA/JACK + Rust stable). Verified locally and in GitHub Actions (Ubuntu). |
| **Windows (MSYS2)** | **Experimental (untested)** | Praat 7's unified `Makefile`, PortAudio backend, and Motif/Win32 timer hooks are in place; requires MSYS2 + matching GNU/LLVM Rust target (`x86_64-pc-windows-gnu`) and has not yet been tested on a Windows runner. |
| **macOS (Apple Silicon / Intel)** | **Code-ready (Xcode wiring needed)** | `Voxalys.cpp` includes native Cocoa (`CFRunLoopTimer`) and PortAudio hooks and `voxalys_rust` builds natively with `cargo build --release`, but `praat.xcodeproj` has not yet been updated to link `Voxalys.cpp` and `libvoxalys_rust.a`. |

> **Need Voxalys on macOS or Windows without compiling Praat?**  
> Use the pure-Praat plugin **[`joenaess/plugin_Voxalys`](https://github.com/joenaess/plugin_Voxalys)** in standard Praat on Linux, macOS, and Windows — or try the **JoVoice** Android app at **[hejai.se](https://www.hejai.se/)**.

---

## Building & Testing Locally (Linux)

### Prerequisites
* C/C++17 toolchain (`g++`, `make`, `pkg-config`) and Linux GUI/audio headers (`libgtk-3-dev`, `libasound2-dev`, `libpulse-dev`, `libjack-jackd2-dev`).
* Rust stable toolchain (`rustc`, `cargo`, `rustfmt`, `clippy`).

### Build & Run
```bash
git clone https://github.com/joenaess/praat.github.io.git praat-voxalys
cd praat-voxalys

# 1. Lint & test the Rust DSP static library
cargo fmt --manifest-path external/voxalys_rust/Cargo.toml --check
cargo clippy --manifest-path external/voxalys_rust/Cargo.toml --all-targets -- -D warnings
cargo test --manifest-path external/voxalys_rust/Cargo.toml

# 2. Build Praat 7 with Voxalys Rust statically linked (auto-detects Linux)
make -j"$(nproc)"
./praat --version

# 3. Run native Voxalys regression tests
./praat --run test/dwtools/voxalys.praat

# 4. Launch Praat GUI
./praat
```

---

## Attribution & Licensing

* **Praat** is created and maintained by Paul Boersma & David Weenink (University of Amsterdam, <https://www.fon.hum.uva.nl/praat/>) under the **GNU GPLv3+**.
* **Voxalys** was originally published by Jonas Lindh (*Lindh, J., 2007, Proceedings of Fonetik 2007*).
* The real-time YIN DSP and lock-free ring-buffer architecture are adapted from **JoVoice** by Jonas Lindh / **[hejAI AB](https://www.hejai.se/)** and relicensed here by the author under **GNU GPL-3.0-or-later**.
* All Rust dependencies in `external/voxalys_rust/Cargo.lock` (`ringbuf`, `crossbeam-utils`, `portable-atomic`, `portable-atomic-util`, `approx`, `num-traits`, `autocfg`) are dual-licensed MIT / Apache-2.0, fully compatible with **GPL-3.0-or-later**.
