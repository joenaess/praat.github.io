# Voxalys Native C++/Rust Extensions & JoVoice Core for Praat

This branch (`feat/voxalys-native-vrp`) integrates native **Voxalys** clinical voice analysis, an interactive real-time **PortAudio Voice Range Profile (VRP)** editor, and the pure-Rust **JoVoice** DSP engine directly into Praat as a statically linked C-ABI library (`libvoxalys_rust.a`).

Author: **Jonas Lindh** ([hejAI AB](https://www.hejai.se/))  
License: **GNU General Public License v3.0 or later (`GPL-3.0-or-later`)**

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
├── Voxalys.cpp                 Native Praat analysis, PortAudio VRP live recorder & Table export
└── praat_Voxalys_init.cpp      Praat Objects menu & Sound action registration

test/dwtools/voxalys.praat      Headless Praat regression tests
.github/workflows/voxalys-ci.yml  GitHub Actions CI (Rust + Praat build & tests)
```

---

## Praat Commands Added

### Objects Window (`New -> Voxalys`)
- **`Record Interactive VRP (live)...`**  
  Opens the native `VoxalysVrpRecorder` window backed by PortAudio (`paFloat32` mono) and the lock-free Rust SPSC stream (`VoxalysStreamHandle`):
  - Real-time $(F_0, \text{SPL})$ scatter plotting with chronological color progression (`Blue -> Cyan -> Green -> Magenta -> Red`) and live per-semitone VRP envelope contour.
  - **Start**, **Pause / Resume**, **Clear**, and **Save Sound & VRP to List** (publishes both the full uninterrupted `Sound` object and a `Table` of `(time_s, f0_Hz, f0_st_re100Hz, spl_dB, voiced, quality, phonation)` directly into Praat's Objects window).

### Dynamic `Sound` Action Menu (`Voxalys -`)
- **`Voxalys analyze voice & JoVoice metrics...`**  
  Combines Praat's native cross-correlation pitch (`Sound_to_Pitch_rawCc`), glottal pulse perturbation (`Sound_Pitch_to_PointProcess_cc`), harmonicity (`Sound_to_Harmonicity_cc`), and LTAS slope (`Sound_to_Ltas`) with Deliyski (1993) / MDVP clinical pathology threshold flags and the **JoVoice** Rust DSP summary (including semitone × dB VRP area and phonation quality score).
- **`Voxalys compute VRP trajectory Table...`**  
  Streams any selected `Sound` through the exact same lock-free Rust SPSC streaming VRP engine used by the live recorder, enabling deterministic batch analysis and headless CI verification without a physical microphone.

---

## Building & Testing Locally

### Prerequisites
- C/C++17 toolchain (`g++`, `make`, `pkg-config`) and Linux GUI/audio headers (`libgtk-3-dev`, `libasound2-dev`, `libpulse-dev`, `libjack-jackd2-dev`).
- Rust stable toolchain (`rustc`, `cargo`, `rustfmt`, `clippy`).
- Optional: `cbindgen` (`cargo install cbindgen`) if regenerating `external/voxalys_rust/include/voxalys_rust.h`.

### Build & Test Commands
```bash
# 1. Lint and unit-test the Rust static library
cargo fmt --manifest-path external/voxalys_rust/Cargo.toml --check
cargo clippy --manifest-path external/voxalys_rust/Cargo.toml --all-targets -- -D warnings
cargo test --manifest-path external/voxalys_rust/Cargo.toml

# 2. Build Praat with Voxalys Rust statically linked
cp makefiles/makefile.defs.linux.pulse-gcc.LE makefile.defs
make -j"$(nproc)"
./praat --version

# 3. Run native Voxalys regression tests
./praat --run test/dwtools/voxalys.praat
```

---

## Attribution & Licensing

- **Praat** is created and maintained by Paul Boersma & David Weenink (University of Amsterdam, <https://www.fon.hum.uva.nl/praat/>) under the **GNU GPLv3+**.
- **Voxalys** was originally published by Jonas Lindh (*Lindh, J., 2007, "Voxalys – a Pedagogical Praat Plugin for Voice Analysis", Proceedings of Fonetik 2007*).
- The real-time YIN DSP and lock-free ring-buffer architecture are adapted from **JoVoice** by Jonas Lindh / **[hejAI AB](https://www.hejai.se/)** and relicensed here by the author under **GNU GPL-3.0-or-later**.
- All Rust dependencies in `external/voxalys_rust/Cargo.lock` (`ringbuf`, `crossbeam-utils`, `portable-atomic`, `portable-atomic-util`, `approx`, `num-traits`, `autocfg`) are dual-licensed MIT / Apache-2.0, fully compatible with **GPL-3.0-or-later**.
