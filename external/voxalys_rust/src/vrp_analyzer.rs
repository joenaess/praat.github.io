// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2007-2026 Jonas Lindh

//! Incremental Voice Range Profile (VRP) frame analyzer.
//!
//! Consumes newly arriving mono audio samples in a sliding window (default
//! ~45 ms window, 10 ms hop) and emits [`VoxalysVrpPoint`] measurements with
//! zero per-frame heap allocations in steady state.

use crate::jovoice_bridge::{VoxalysModelHandle, VoxalysPhonationClass, YinScratch};

/// Single point along a real-time or batch Voice Range Profile trajectory.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct VoxalysVrpPoint {
    pub time_sec: f64,
    pub f0_hz: f32,
    pub spl_db: f32,
    pub voiced: bool,
    pub quality_score: f32,
    pub phonation_class: VoxalysPhonationClass,
}

/// Incremental sliding-window VRP analyzer.
pub struct VrpAnalyzer {
    sample_rate: f32,
    window_samples: usize,
    hop_samples: usize,
    model: VoxalysModelHandle,
    scratch: YinScratch,
    sliding_window: Vec<f32>,
    filled: usize,
    since_last_hop: usize,
    total_samples_consumed: u64,
}

impl VrpAnalyzer {
    /// Constructs a new incremental analyzer configured for the given sample rate and pitch bounds.
    pub fn new(
        sample_rate: f32,
        f0_min_hz: f32,
        f0_max_hz: f32,
        spl_calibration_offset_db: f32,
    ) -> Result<Self, String> {
        if !sample_rate.is_finite() || sample_rate < 4000.0 {
            return Err(format!("Invalid sample rate: {sample_rate}"));
        }
        let model = VoxalysModelHandle::new(f0_min_hz, f0_max_hz, spl_calibration_offset_db)?;
        let min_win_for_f0 = ((2.2 * sample_rate / f0_min_hz).ceil() as usize).max(512);
        let default_win = ((sample_rate * 0.040).round() as usize).max(512);
        let window_samples = default_win.max(min_win_for_f0);
        let hop_samples = ((sample_rate * 0.010).round() as usize).max(1);

        let mut analyzer = Self {
            sample_rate,
            window_samples,
            hop_samples,
            model,
            scratch: YinScratch::default(),
            sliding_window: vec![0.0f32; window_samples],
            filled: 0,
            since_last_hop: 0,
            total_samples_consumed: 0,
        };
        // Pre-warm YIN scratch buffers so steady-state analysis does zero allocations.
        let _ = analyzer.model.analyze_frame(
            &analyzer.sliding_window,
            analyzer.sample_rate,
            0.0,
            &mut analyzer.scratch,
        );
        Ok(analyzer)
    }

    /// Resets the internal sliding window and timestamp counter.
    pub fn reset(&mut self) {
        self.sliding_window.fill(0.0);
        self.filled = 0;
        self.since_last_hop = 0;
        self.total_samples_consumed = 0;
    }

    /// Returns the window size in samples.
    pub fn window_samples(&self) -> usize {
        self.window_samples
    }

    /// Returns the hop size in samples.
    pub fn hop_samples(&self) -> usize {
        self.hop_samples
    }

    /// Feeds incoming samples and appends newly emitted VRP points into `out`.
    pub fn process_samples(&mut self, samples: &[f32], out: &mut Vec<VoxalysVrpPoint>) {
        let mut cursor = 0usize;
        while cursor < samples.len() {
            if self.filled < self.window_samples {
                let needed = self.window_samples - self.filled;
                let take = needed.min(samples.len() - cursor);
                self.sliding_window[self.filled..self.filled + take]
                    .copy_from_slice(&samples[cursor..cursor + take]);
                self.filled += take;
                self.total_samples_consumed += take as u64;
                cursor += take;

                if self.filled == self.window_samples {
                    out.push(self.emit_current_point());
                    self.since_last_hop = 0;
                }
            } else {
                let needed = self.hop_samples - self.since_last_hop;
                let take = needed.min(samples.len() - cursor);
                if take >= self.window_samples {
                    let start = cursor + take - self.window_samples;
                    self.sliding_window
                        .copy_from_slice(&samples[start..cursor + take]);
                } else {
                    self.sliding_window.copy_within(take.., 0);
                    let dst_start = self.window_samples - take;
                    self.sliding_window[dst_start..]
                        .copy_from_slice(&samples[cursor..cursor + take]);
                }
                self.total_samples_consumed += take as u64;
                self.since_last_hop += take;
                cursor += take;

                if self.since_last_hop >= self.hop_samples {
                    out.push(self.emit_current_point());
                    self.since_last_hop = 0;
                }
            }
        }
    }

    fn emit_current_point(&mut self) -> VoxalysVrpPoint {
        let center_sample = self
            .total_samples_consumed
            .saturating_sub((self.window_samples / 2) as u64);
        let time_sec = center_sample as f64 / self.sample_rate as f64;
        let frame = self.model.analyze_frame(
            &self.sliding_window,
            self.sample_rate,
            time_sec,
            &mut self.scratch,
        );
        VoxalysVrpPoint {
            time_sec: frame.time_sec,
            f0_hz: frame.f0_hz,
            spl_db: frame.spl_db,
            voiced: frame.voiced,
            quality_score: frame.quality_score,
            phonation_class: frame.phonation_class,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::PI;

    #[test]
    fn incremental_chunks_match_full_buffer() {
        let sr = 16_000.0f32;
        let signal: Vec<f32> = (0..16_000)
            .map(|i| 0.6 * (2.0 * PI * 220.0 * i as f32 / sr).sin())
            .collect();

        let mut a_full = VrpAnalyzer::new(sr, 65.0, 800.0, 94.0).unwrap();
        let mut pts_full = Vec::new();
        a_full.process_samples(&signal, &mut pts_full);

        let mut a_chunked = VrpAnalyzer::new(sr, 65.0, 800.0, 94.0).unwrap();
        let mut pts_chunked = Vec::new();
        for chunk in signal.chunks(137) {
            a_chunked.process_samples(chunk, &mut pts_chunked);
        }

        assert_eq!(pts_full.len(), pts_chunked.len());
        assert!(!pts_full.is_empty());
        for (p1, p2) in pts_full.iter().zip(pts_chunked.iter()) {
            assert_eq!(p1, p2);
            assert!(p1.voiced);
            assert!((p1.f0_hz - 220.0).abs() < 1.0);
        }
    }
}
