// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2007-2026 Jonas Lindh

//! JoVoice DSP & voice-quality inference bridge.
//!
//! Adapts the pure-Rust YIN fundamental frequency estimator, RMS/SPL level
//! detector, and voicing/quality feature extractors from JoVoice (`core_engine`)
//! behind an opaque [`VoxalysModelHandle`] suitable for C/C++ FFI.

/// Lowest pitch supported by default (below C2 at 65.4 Hz).
pub const MIN_PITCH_HZ: f32 = 50.0;
/// Highest pitch supported by default (above C6 at 1046.5 Hz).
pub const MAX_PITCH_HZ: f32 = 1100.0;
/// Default SPL calibration offset added to dBFS (full-scale sine ≈ 91 dB SPL).
pub const DEFAULT_SPL_CALIBRATION_DB: f32 = 94.0;
/// Default silence gate in dBFS matching JoVoice's real-time engine.
pub const DEFAULT_SILENCE_GATE_DBFS: f32 = -55.0;

/// Clinical phonation quality classification emitted per frame.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VoxalysPhonationClass {
    /// Silent or unvoiced frame.
    Unvoiced = 0,
    /// Periodic, balanced modal phonation.
    Modal = 1,
    /// Voiced frame with elevated high-frequency noise / lower periodicity.
    Breathy = 2,
    /// High-intensity, strong high-frequency harmonic energy.
    Pressed = 3,
    /// Irregular / perturbed voiced frame (elevated cycle aperiodicity).
    Rough = 4,
}

/// Per-frame acoustic & JoVoice model output.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct VoxalysFrameResult {
    pub time_sec: f64,
    pub f0_hz: f32,
    pub rms_dbfs: f32,
    pub spl_db: f32,
    pub confidence: f32,
    pub quality_score: f32,
    pub voiced: bool,
    pub phonation_class: VoxalysPhonationClass,
}

/// Aggregated acoustic & VRP summary across an entire audio buffer.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct VoxalysSummaryResult {
    pub frame_count: u64,
    pub voiced_frame_count: u64,
    pub voiced_fraction: f64,
    pub f0_mean_hz: f64,
    pub f0_median_hz: f64,
    pub f0_min_hz: f64,
    pub f0_max_hz: f64,
    pub f0_sd_hz: f64,
    pub f0_range_semitones: f64,
    pub spl_mean_db: f64,
    pub spl_min_db: f64,
    pub spl_max_db: f64,
    pub spl_dynamic_range_db: f64,
    pub mean_confidence: f64,
    pub mean_quality_score: f64,
    pub vrp_area_st_db: f64,
}

impl Default for VoxalysSummaryResult {
    fn default() -> Self {
        Self {
            frame_count: 0,
            voiced_frame_count: 0,
            voiced_fraction: 0.0,
            f0_mean_hz: 0.0,
            f0_median_hz: 0.0,
            f0_min_hz: 0.0,
            f0_max_hz: 0.0,
            f0_sd_hz: 0.0,
            f0_range_semitones: 0.0,
            spl_mean_db: -100.0,
            spl_min_db: -100.0,
            spl_max_db: -100.0,
            spl_dynamic_range_db: 0.0,
            mean_confidence: 0.0,
            mean_quality_score: 0.0,
            vrp_area_st_db: 0.0,
        }
    }
}

/// Detailed YIN estimate for a single analysis frame.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct YinEstimate {
    pub f0_hz: f32,
    pub confidence: f32,
    pub voiced: bool,
}

/// Reusable scratch buffers for allocation-free YIN computation in streaming mode.
#[derive(Debug, Default, Clone)]
pub struct YinScratch {
    d: Vec<f32>,
    d_prime: Vec<f32>,
}

/// Opaque configuration & model handle for JoVoice/Voxalys analysis.
#[derive(Debug, Clone)]
pub struct VoxalysModelHandle {
    pub f0_min_hz: f32,
    pub f0_max_hz: f32,
    pub spl_calibration_offset_db: f32,
    pub silence_gate_dbfs: f32,
}

impl VoxalysModelHandle {
    /// Creates a new model handle with validated clinical frequency bounds.
    pub fn new(
        f0_min_hz: f32,
        f0_max_hz: f32,
        spl_calibration_offset_db: f32,
    ) -> Result<Self, String> {
        if !f0_min_hz.is_finite()
            || !f0_max_hz.is_finite()
            || f0_min_hz < 20.0
            || f0_max_hz <= f0_min_hz
        {
            return Err(format!(
                "Invalid F0 range: min={f0_min_hz}, max={f0_max_hz}"
            ));
        }
        if !spl_calibration_offset_db.is_finite() {
            return Err("Invalid SPL calibration offset".to_string());
        }
        Ok(Self {
            f0_min_hz,
            f0_max_hz,
            spl_calibration_offset_db,
            silence_gate_dbfs: DEFAULT_SILENCE_GATE_DBFS,
        })
    }

    /// Evaluates a single audio frame and returns structured acoustic/quality metrics.
    pub fn analyze_frame(
        &self,
        samples: &[f32],
        sample_rate: f32,
        time_sec: f64,
        scratch: &mut YinScratch,
    ) -> VoxalysFrameResult {
        let rms_dbfs = calculate_rms_dbfs(samples);
        let spl_db = rms_dbfs + self.spl_calibration_offset_db;

        if rms_dbfs <= self.silence_gate_dbfs {
            return VoxalysFrameResult {
                time_sec,
                f0_hz: 0.0,
                rms_dbfs,
                spl_db,
                confidence: 0.0,
                quality_score: 0.0,
                voiced: false,
                phonation_class: VoxalysPhonationClass::Unvoiced,
            };
        }

        let yin = estimate_pitch_yin_with_scratch(
            samples,
            sample_rate,
            self.f0_min_hz,
            self.f0_max_hz,
            scratch,
        );
        if !yin.voiced {
            return VoxalysFrameResult {
                time_sec,
                f0_hz: 0.0,
                rms_dbfs,
                spl_db,
                confidence: yin.confidence,
                quality_score: 0.0,
                voiced: false,
                phonation_class: VoxalysPhonationClass::Unvoiced,
            };
        }

        let hf_ratio = compute_high_freq_energy_ratio(samples);
        let quality_score =
            (0.75 * yin.confidence + 0.25 * (1.0 - hf_ratio.clamp(0.0, 1.0))).clamp(0.0, 1.0);
        let phonation_class = classify_phonation(yin.confidence, hf_ratio, spl_db);

        VoxalysFrameResult {
            time_sec,
            f0_hz: yin.f0_hz,
            rms_dbfs,
            spl_db,
            confidence: yin.confidence,
            quality_score,
            voiced: true,
            phonation_class,
        }
    }

    /// Analyzes an entire mono audio buffer in 10 ms hops and aggregates summary statistics.
    pub fn analyze_buffer(
        &self,
        samples: &[f32],
        sample_rate: f32,
    ) -> Result<(VoxalysSummaryResult, Vec<VoxalysFrameResult>), String> {
        if !sample_rate.is_finite() || sample_rate < 4000.0 {
            return Err(format!("Invalid sample rate: {sample_rate}"));
        }
        if samples.is_empty() {
            return Ok((VoxalysSummaryResult::default(), Vec::new()));
        }

        let min_win_for_f0 = ((2.2 * sample_rate / self.f0_min_hz).ceil() as usize).max(512);
        let default_win = ((sample_rate * 0.040).round() as usize).max(512);
        let win_len = default_win.max(min_win_for_f0);
        let hop_len = ((sample_rate * 0.010).round() as usize).max(1);

        let mut scratch = YinScratch::default();
        let mut frames = Vec::new();

        if samples.len() < win_len {
            let t = (samples.len() as f64 / sample_rate as f64) * 0.5;
            frames.push(self.analyze_frame(samples, sample_rate, t, &mut scratch));
        } else {
            let mut offset = 0usize;
            while offset + win_len <= samples.len() {
                let slice = &samples[offset..offset + win_len];
                let t = (offset as f64 + win_len as f64 * 0.5) / sample_rate as f64;
                frames.push(self.analyze_frame(slice, sample_rate, t, &mut scratch));
                offset += hop_len;
            }
        }

        let summary = summarize_frames(&frames);
        Ok((summary, frames))
    }
}

/// Root-mean-square level in dBFS; `-100.0` for silence.
pub fn calculate_rms_dbfs(buffer: &[f32]) -> f32 {
    if buffer.is_empty() {
        return -100.0;
    }
    let sum_squares: f64 = buffer.iter().map(|&s| (s as f64) * (s as f64)).sum();
    let rms = (sum_squares / buffer.len() as f64).sqrt();
    if rms <= 1e-6 {
        -100.0
    } else {
        (20.0 * rms.log10()) as f32
    }
}

/// Ratio of first-difference (high-frequency) energy to total signal energy.
///
/// Serves as a lightweight, zero-FFT indicator of breathiness / spectral tilt.
fn compute_high_freq_energy_ratio(buffer: &[f32]) -> f32 {
    if buffer.len() < 2 {
        return 0.0;
    }
    let mut total_energy = 0.0f64;
    let mut diff_energy = 0.0f64;
    for i in 1..buffer.len() {
        let s = buffer[i] as f64;
        let d = (buffer[i] - buffer[i - 1]) as f64;
        total_energy += s * s;
        diff_energy += d * d;
    }
    if total_energy <= 1e-12 {
        0.0
    } else {
        // Normalize by 4.0 (max ratio for Nyquist alternating signal).
        ((diff_energy / total_energy) * 0.25).clamp(0.0, 1.0) as f32
    }
}

/// Classifies voiced frames into clinical phonation categories.
fn classify_phonation(confidence: f32, hf_ratio: f32, spl_db: f32) -> VoxalysPhonationClass {
    if confidence < 0.72 {
        VoxalysPhonationClass::Rough
    } else if hf_ratio > 0.35 && spl_db < 72.0 {
        VoxalysPhonationClass::Breathy
    } else if spl_db > 88.0 && hf_ratio > 0.18 {
        VoxalysPhonationClass::Pressed
    } else {
        VoxalysPhonationClass::Modal
    }
}

/// JoVoice YIN Pitch Estimation Algorithm with Sub-Hz Parabolic Interpolation & VAD Rejection.
pub fn estimate_pitch_yin(buffer: &[f32], sample_rate: f32) -> f32 {
    let mut scratch = YinScratch::default();
    estimate_pitch_yin_with_scratch(
        buffer,
        sample_rate,
        MIN_PITCH_HZ,
        MAX_PITCH_HZ,
        &mut scratch,
    )
    .f0_hz
}

/// Detailed YIN pitch estimation using caller-provided scratch buffers (zero heap allocation
/// once scratch capacity has been warmed up).
pub fn estimate_pitch_yin_with_scratch(
    buffer: &[f32],
    sample_rate: f32,
    min_freq: f32,
    max_freq: f32,
    scratch: &mut YinScratch,
) -> YinEstimate {
    let unvoiced = YinEstimate {
        f0_hz: 0.0,
        confidence: 0.0,
        voiced: false,
    };

    let n = buffer.len();
    if n < 256
        || !sample_rate.is_finite()
        || sample_rate <= 0.0
        || min_freq <= 0.0
        || max_freq <= min_freq
    {
        return unvoiced;
    }

    let min_lag = ((sample_rate / max_freq).floor() as usize).max(2);
    let max_lag = ((sample_rate / min_freq).ceil() as usize).min(n / 2);
    if max_lag <= min_lag + 2 {
        return unvoiced;
    }

    scratch.d.clear();
    scratch.d.resize(max_lag, 0.0);
    scratch.d_prime.clear();
    scratch.d_prime.resize(max_lag, 1.0);

    // 1. Difference function d(tau) with step_by(2) optimization matching JoVoice DSP.
    let search_window = n - max_lag;
    for tau in 1..max_lag {
        let mut diff_sum = 0.0f32;
        let mut i = 0;
        while i < search_window {
            let diff = buffer[i] - buffer[i + tau];
            diff_sum += diff * diff;
            i += 2;
        }
        scratch.d[tau] = diff_sum * 2.0;
    }

    // 2. Cumulative Mean Normalized Difference Function d'(tau).
    scratch.d_prime[0] = 1.0;
    let mut running_sum = 0.0f32;
    for tau in 1..max_lag {
        running_sum += scratch.d[tau];
        if running_sum > 1e-6 {
            scratch.d_prime[tau] = scratch.d[tau] * (tau as f32) / running_sum;
        } else {
            scratch.d_prime[tau] = 1.0;
        }
    }

    // 3. Absolute thresholding for first local minimum under harmonicity threshold.
    let threshold = 0.20f32;
    let mut best_tau = 0usize;
    let mut best_val = 1.0f32;

    for tau in min_lag..(max_lag - 1) {
        if scratch.d_prime[tau] < threshold {
            let mut curr = tau;
            while curr + 1 < max_lag - 1 && scratch.d_prime[curr + 1] < scratch.d_prime[curr] {
                curr += 1;
            }
            best_tau = curr;
            best_val = scratch.d_prime[curr];
            break;
        }
    }

    // Fallback global minimum with VAD harmonicity rejection (> 0.40 is unvoiced noise).
    if best_tau == 0 {
        for (tau, &value) in scratch
            .d_prime
            .iter()
            .enumerate()
            .take(max_lag - 1)
            .skip(min_lag)
        {
            if value < best_val {
                best_val = value;
                best_tau = tau;
            }
        }
        if best_val > 0.40 {
            return YinEstimate {
                f0_hz: 0.0,
                confidence: (1.0 - best_val).clamp(0.0, 1.0),
                voiced: false,
            };
        }
    }

    if best_tau == 0 || best_tau <= min_lag || best_tau >= max_lag - 1 {
        return unvoiced;
    }

    // 4. Parabolic interpolation for sub-sample pitch accuracy.
    let s0 = scratch.d_prime[best_tau - 1];
    let s1 = scratch.d_prime[best_tau];
    let s2 = scratch.d_prime[best_tau + 1];
    let denom = 2.0 * (2.0 * s1 - s0 - s2);
    let delta = if denom.abs() > 1e-6 {
        ((s2 - s0) / denom).clamp(-0.5, 0.5)
    } else {
        0.0
    };

    let refined_tau = (best_tau as f32) + delta;
    if refined_tau <= 0.0 {
        return unvoiced;
    }

    let pitch = sample_rate / refined_tau;
    if pitch >= min_freq && pitch <= max_freq {
        YinEstimate {
            f0_hz: pitch,
            confidence: (1.0 - best_val).clamp(0.0, 1.0),
            voiced: true,
        }
    } else {
        unvoiced
    }
}

/// Aggregates frame results into a clinical summary including semitone × dB VRP area.
pub fn summarize_frames(frames: &[VoxalysFrameResult]) -> VoxalysSummaryResult {
    if frames.is_empty() {
        return VoxalysSummaryResult::default();
    }

    let frame_count = frames.len() as u64;
    let mut voiced_f0: Vec<f64> = Vec::new();
    let mut voiced_spl: Vec<f64> = Vec::new();
    let mut sum_conf = 0.0f64;
    let mut sum_qual = 0.0f64;

    // 1-semitone bin min/max SPL envelope for VRP area estimation (re 100 Hz).
    let mut st_bins: std::collections::BTreeMap<i32, (f64, f64)> =
        std::collections::BTreeMap::new();

    for f in frames {
        if f.voiced && f.f0_hz > 0.0 {
            let f0 = f.f0_hz as f64;
            let spl = f.spl_db as f64;
            voiced_f0.push(f0);
            voiced_spl.push(spl);
            sum_conf += f.confidence as f64;
            sum_qual += f.quality_score as f64;

            let st_bin = (12.0 * (f0 / 100.0).log2()).round() as i32;
            st_bins
                .entry(st_bin)
                .and_modify(|(mn, mx)| {
                    if spl < *mn {
                        *mn = spl;
                    }
                    if spl > *mx {
                        *mx = spl;
                    }
                })
                .or_insert((spl, spl));
        }
    }

    let voiced_count = voiced_f0.len() as u64;
    if voiced_count == 0 {
        return VoxalysSummaryResult {
            frame_count,
            ..VoxalysSummaryResult::default()
        };
    }

    voiced_f0.sort_by(|a, b| a.total_cmp(b));
    let n = voiced_f0.len() as f64;
    let f0_mean_hz = voiced_f0.iter().sum::<f64>() / n;
    let f0_median_hz = if voiced_f0.len() % 2 == 1 {
        voiced_f0[voiced_f0.len() / 2]
    } else {
        0.5 * (voiced_f0[voiced_f0.len() / 2 - 1] + voiced_f0[voiced_f0.len() / 2])
    };
    let f0_min_hz = voiced_f0[0];
    let f0_max_hz = voiced_f0[voiced_f0.len() - 1];
    let f0_var = voiced_f0
        .iter()
        .map(|&v| {
            let d = v - f0_mean_hz;
            d * d
        })
        .sum::<f64>()
        / n;
    let f0_sd_hz = f0_var.sqrt();
    let f0_range_semitones = if f0_min_hz > 0.0 {
        12.0 * (f0_max_hz / f0_min_hz).log2()
    } else {
        0.0
    };

    let spl_mean_db = voiced_spl.iter().sum::<f64>() / n;
    let spl_min_db = voiced_spl.iter().copied().fold(f64::INFINITY, f64::min);
    let spl_max_db = voiced_spl.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let spl_dynamic_range_db = (spl_max_db - spl_min_db).max(0.0);

    let vrp_area_st_db: f64 = st_bins.values().map(|(mn, mx)| (mx - mn).max(0.0)).sum();

    VoxalysSummaryResult {
        frame_count,
        voiced_frame_count: voiced_count,
        voiced_fraction: voiced_count as f64 / frame_count as f64,
        f0_mean_hz,
        f0_median_hz,
        f0_min_hz,
        f0_max_hz,
        f0_sd_hz,
        f0_range_semitones,
        spl_mean_db,
        spl_min_db,
        spl_max_db,
        spl_dynamic_range_db,
        mean_confidence: sum_conf / n,
        mean_quality_score: sum_qual / n,
        vrp_area_st_db,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use approx::assert_abs_diff_eq;
    use std::f32::consts::PI;

    const SR: f32 = 16_000.0;

    fn sine(freq: f32, sample_rate: f32, len: usize, amp: f32) -> Vec<f32> {
        (0..len)
            .map(|i| amp * (2.0 * PI * freq * i as f32 / sample_rate).sin())
            .collect()
    }

    fn harmonic_pulse(freq: f32, sample_rate: f32, len: usize) -> Vec<f32> {
        let harmonics = (((sample_rate / 2.0) / freq) as usize).min(12);
        let mut buf: Vec<f32> = (0..len)
            .map(|i| {
                let t = i as f32 / sample_rate;
                (1..=harmonics)
                    .map(|k| (2.0 * PI * freq * k as f32 * t).sin() / k as f32)
                    .sum::<f32>()
            })
            .collect();
        let peak = buf.iter().map(|v| v.abs()).fold(0.0f32, f32::max).max(1e-6);
        for v in &mut buf {
            *v = (*v / peak) * 0.7;
        }
        buf
    }

    fn white_noise(len: usize, seed: u32) -> Vec<f32> {
        let mut state = seed;
        (0..len)
            .map(|_| {
                state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                (state >> 8) as f32 / (1u32 << 24) as f32 - 0.5
            })
            .collect()
    }

    #[test]
    fn rms_and_spl_calibration() {
        assert_eq!(calculate_rms_dbfs(&[]), -100.0);
        assert_eq!(calculate_rms_dbfs(&[0.0; 1024]), -100.0);
        let db = calculate_rms_dbfs(&sine(440.0, SR, 1024, 1.0));
        assert_abs_diff_eq!(db, -3.0103, epsilon = 0.05);

        let model = VoxalysModelHandle::new(50.0, 1100.0, 94.0).unwrap();
        let mut scratch = YinScratch::default();
        let frame = model.analyze_frame(&sine(220.0, SR, 1024, 1.0), SR, 0.0, &mut scratch);
        assert!(frame.voiced);
        assert_abs_diff_eq!(frame.spl_db, 90.99, epsilon = 0.15);
    }

    #[test]
    fn pitch_accuracy_within_1_hz_across_vocal_range() {
        for &target in &[65.41, 82.41, 150.0, 261.63, 440.0] {
            let sig = harmonic_pulse(target, SR, 1024);
            let f0 = estimate_pitch_yin(&sig, SR);
            assert!(
                (f0 - target).abs() < 1.0,
                "expected {target} Hz +/- 1 Hz, got {f0} Hz"
            );
        }
        // High soprano C6 at 44.1 kHz
        let sig_c6 = sine(1046.5, 44_100.0, 2048, 0.7);
        let f0_c6 = estimate_pitch_yin(&sig_c6, 44_100.0);
        assert!((f0_c6 - 1046.5).abs() < 2.0, "got {f0_c6} Hz");
    }

    #[test]
    fn rejects_noise_and_silence() {
        for seed in 1..=5 {
            assert_eq!(estimate_pitch_yin(&white_noise(1024, seed), SR), 0.0);
        }
        assert_eq!(estimate_pitch_yin(&[0.0; 1024], SR), 0.0);
    }

    #[test]
    fn model_buffer_analysis_is_deterministic() {
        let model = VoxalysModelHandle::new(65.0, 800.0, 94.0).unwrap();
        let sig = harmonic_pulse(150.0, 44_100.0, 44_100);
        let (s1, f1) = model.analyze_buffer(&sig, 44_100.0).unwrap();
        let (s2, f2) = model.analyze_buffer(&sig, 44_100.0).unwrap();
        assert_eq!(s1, s2);
        assert_eq!(f1, f2);
        assert!(s1.voiced_fraction > 0.9);
        assert!((s1.f0_mean_hz - 150.0).abs() < 1.0);
        assert!(s1.mean_quality_score > 0.7);
    }
}
