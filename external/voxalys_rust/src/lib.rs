// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2007-2026 Jonas Lindh

//! Voxalys real-time Voice Range Profile (VRP) and JoVoice DSP engine for Praat.
//!
//! Compiled as a C-ABI static library (`libvoxalys_rust.a`) with zero dynamic
//! runtime dependencies so Praat remains a self-contained single executable.

pub mod ffi;
pub mod jovoice_bridge;
pub mod ring_buffer;
pub mod vrp_analyzer;
