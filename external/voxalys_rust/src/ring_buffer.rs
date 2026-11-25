// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2007-2026 Jonas Lindh

//! Lock-free single-producer single-consumer (SPSC) audio ring buffer for
//! real-time audio callbacks.
//!
//! The producer side ([`VoxalysStreamHandle::push_samples`]) performs zero heap
//! allocations and acquires zero mutexes, making it safe to call directly from
//! PortAudio / CoreAudio / ALSA / PulseAudio / JACK real-time callbacks.

use std::cell::UnsafeCell;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU64, Ordering};

use ringbuf::traits::{Consumer, Producer, Split};
use ringbuf::{HeapCons, HeapProd, HeapRb};

use crate::vrp_analyzer::{VoxalysVrpPoint, VrpAnalyzer};

/// Consumer-side state accessed exclusively by the GUI / polling thread.
struct ConsumerState {
    consumer: HeapCons<f32>,
    analyzer: VrpAnalyzer,
    drain_buf: Vec<f32>,
    pending_points: VecDeque<VoxalysVrpPoint>,
    temp_points: Vec<VoxalysVrpPoint>,
}

/// Lock-free SPSC audio stream combining a ring buffer with an incremental VRP analyzer.
///
/// # Threading contract
/// - At most one producer thread (e.g., PortAudio callback) may call [`Self::push_samples`].
/// - At most one consumer thread (e.g., Praat GUI timer) may call [`Self::poll_points`]
///   or [`Self::reset`].
pub struct VoxalysStreamHandle {
    producer: UnsafeCell<HeapProd<f32>>,
    consumer_state: UnsafeCell<ConsumerState>,
    dropped_samples: AtomicU64,
    pushed_samples: AtomicU64,
}

// SAFETY: `producer` is only ever accessed by the single audio producer thread,
// while `consumer_state` is only ever accessed by the single consumer/UI thread.
// `HeapProd<f32>` and `HeapCons<f32>` synchronize internally via lock-free atomics.
unsafe impl Send for VoxalysStreamHandle {}
unsafe impl Sync for VoxalysStreamHandle {}

impl VoxalysStreamHandle {
    /// Creates a new SPSC audio stream and VRP analyzer.
    pub fn new(
        sample_rate: f32,
        capacity_samples: usize,
        f0_min_hz: f32,
        f0_max_hz: f32,
        spl_calibration_offset_db: f32,
    ) -> Result<Self, String> {
        let cap = capacity_samples.max(4096);
        let analyzer =
            VrpAnalyzer::new(sample_rate, f0_min_hz, f0_max_hz, spl_calibration_offset_db)?;
        let rb = HeapRb::<f32>::new(cap);
        let (producer, consumer) = rb.split();
        let drain_chunk = analyzer.window_samples().max(2048);

        Ok(Self {
            producer: UnsafeCell::new(producer),
            consumer_state: UnsafeCell::new(ConsumerState {
                consumer,
                analyzer,
                drain_buf: vec![0.0f32; drain_chunk],
                pending_points: VecDeque::with_capacity(256),
                temp_points: Vec::with_capacity(128),
            }),
            dropped_samples: AtomicU64::new(0),
            pushed_samples: AtomicU64::new(0),
        })
    }

    /// Lock-free, zero-allocation push from the audio callback thread.
    ///
    /// Returns the number of samples written into the ring buffer. Any samples
    /// that did not fit increment the atomic dropped-sample counter.
    ///
    /// # Safety
    /// Must only be called from a single producer thread at a time.
    pub unsafe fn push_samples(&self, samples: &[f32]) -> usize {
        // SAFETY: SPSC contract guarantees exclusive access to `producer` on this thread.
        let prod = unsafe { &mut *self.producer.get() };
        let written = prod.push_slice(samples);
        self.pushed_samples
            .fetch_add(written as u64, Ordering::Relaxed);
        if written < samples.len() {
            self.dropped_samples
                .fetch_add((samples.len() - written) as u64, Ordering::Relaxed);
        }
        written
    }

    /// Drains available audio from the ring buffer, runs incremental VRP analysis,
    /// and writes up to `out.len()` resulting points.
    ///
    /// # Safety
    /// Must only be called from the single consumer/UI thread.
    pub unsafe fn poll_points(&self, out: &mut [VoxalysVrpPoint]) -> usize {
        // SAFETY: SPSC contract guarantees exclusive access to `consumer_state` on this thread.
        let state = unsafe { &mut *self.consumer_state.get() };

        loop {
            let n = state.consumer.pop_slice(&mut state.drain_buf);
            if n == 0 {
                break;
            }
            state.temp_points.clear();
            state
                .analyzer
                .process_samples(&state.drain_buf[..n], &mut state.temp_points);
            state.pending_points.extend(state.temp_points.drain(..));
        }

        let mut count = 0usize;
        while count < out.len() {
            if let Some(pt) = state.pending_points.pop_front() {
                out[count] = pt;
                count += 1;
            } else {
                break;
            }
        }
        count
    }

    /// Resets consumer analysis state and drains any unread ring-buffer samples.
    ///
    /// # Safety
    /// Must only be called from the consumer thread when the producer is idle or stopped.
    pub unsafe fn reset(&self) {
        // SAFETY: SPSC contract guarantees exclusive access to `consumer_state`.
        let state = unsafe { &mut *self.consumer_state.get() };
        while state.consumer.pop_slice(&mut state.drain_buf) > 0 {}
        state.pending_points.clear();
        state.temp_points.clear();
        state.analyzer.reset();
        self.dropped_samples.store(0, Ordering::Relaxed);
        self.pushed_samples.store(0, Ordering::Relaxed);
    }

    /// Returns total samples dropped due to ring-buffer overflow.
    pub fn dropped_samples(&self) -> u64 {
        self.dropped_samples.load(Ordering::Relaxed)
    }

    /// Returns total samples accepted into the ring buffer.
    pub fn pushed_samples(&self) -> u64 {
        self.pushed_samples.load(Ordering::Relaxed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::PI;
    use std::sync::Arc;
    use std::thread;

    #[test]
    fn concurrent_producer_consumer_wraparound_preserves_pitch() {
        let sr = 16_000.0f32;
        // Small ring buffer (4096 samples) with 64,000 samples pushed -> >15 wrap-arounds.
        let stream = Arc::new(VoxalysStreamHandle::new(sr, 4096, 65.0, 800.0, 94.0).unwrap());
        let total_samples = 64_000usize;
        let freq = 200.0f32;

        let prod_stream = Arc::clone(&stream);
        let producer = thread::spawn(move || {
            let chunk_size = 256usize;
            let mut idx = 0usize;
            let mut buf = vec![0.0f32; chunk_size];
            while idx < total_samples {
                let n = chunk_size.min(total_samples - idx);
                for (j, sample) in buf[..n].iter_mut().enumerate() {
                    *sample = 0.65 * (2.0 * PI * freq * (idx + j) as f32 / sr).sin();
                }
                let mut sent = 0usize;
                while sent < n {
                    // SAFETY: Only this thread produces.
                    let w = unsafe { prod_stream.push_samples(&buf[sent..n]) };
                    sent += w;
                    if w == 0 {
                        thread::yield_now();
                    }
                }
                idx += n;
            }
        });

        let mut all_points = Vec::new();
        let mut out = [VoxalysVrpPoint {
            time_sec: 0.0,
            f0_hz: 0.0,
            spl_db: 0.0,
            voiced: false,
            quality_score: 0.0,
            phonation_class: crate::jovoice_bridge::VoxalysPhonationClass::Unvoiced,
        }; 64];

        while !producer.is_finished() {
            // SAFETY: Only this thread consumes.
            let n = unsafe { stream.poll_points(&mut out) };
            all_points.extend_from_slice(&out[..n]);
            if n == 0 {
                thread::yield_now();
            }
        }
        producer.join().unwrap();

        // Final drain after producer finishes.
        loop {
            // SAFETY: Only this thread consumes.
            let n = unsafe { stream.poll_points(&mut out) };
            if n == 0 {
                break;
            }
            all_points.extend_from_slice(&out[..n]);
        }

        assert_eq!(stream.pushed_samples(), total_samples as u64);
        assert!(all_points.len() >= 380, "got {} points", all_points.len());
        let voiced: Vec<_> = all_points.iter().filter(|p| p.voiced).collect();
        assert!(voiced.len() > 350);
        for p in &voiced {
            assert!((p.f0_hz - freq).abs() < 1.0, "got {} Hz", p.f0_hz);
        }
    }

    #[test]
    fn overflow_increments_dropped_samples_counter() {
        let stream = VoxalysStreamHandle::new(16_000.0, 4096, 65.0, 800.0, 94.0).unwrap();
        let big = vec![0.1f32; 5000];
        // SAFETY: Single producer in test.
        let written = unsafe { stream.push_samples(&big) };
        assert_eq!(written, 4096);
        assert_eq!(stream.dropped_samples(), 904);
    }
}
