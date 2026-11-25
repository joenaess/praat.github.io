/* Voxalys.cpp
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2007-2026 Jonas Lindh
 *
 * Native C++ acoustic analysis, JoVoice Rust DSP bridge, and interactive
 * real-time PortAudio Voice Range Profile (VRP) Editor for Praat.
 */

#include "Voxalys.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "EditorM.h"
#include "Harmonicity.h"
#include "Ltas.h"
#include "Pitch.h"
#include "Pitch_to_PointProcess.h"
#include "PointProcess.h"
#include "Sound_to_Harmonicity.h"
#include "Sound_to_Pitch.h"
#include "VoiceAnalysis.h"
#include "machine.h"

Thing_implement (VoxalysVrpRecorder, Editor, 0);

namespace {

struct ScopedModelHandle {
	VoxalysModelHandle *ptr = nullptr;
	explicit ScopedModelHandle (VoxalysModelHandle *p) noexcept : ptr (p) {}
	~ScopedModelHandle () noexcept {
		if (ptr)
			voxalys_model_free (ptr);
	}
	ScopedModelHandle (const ScopedModelHandle &) = delete;
	ScopedModelHandle & operator= (const ScopedModelHandle &) = delete;
};

struct ScopedStreamHandle {
	VoxalysStreamHandle *ptr = nullptr;
	explicit ScopedStreamHandle (VoxalysStreamHandle *p) noexcept : ptr (p) {}
	~ScopedStreamHandle () noexcept {
		if (ptr)
			voxalys_stream_free (ptr);
	}
	ScopedStreamHandle (const ScopedStreamHandle &) = delete;
	ScopedStreamHandle & operator= (const ScopedStreamHandle &) = delete;
};

conststring32 phonationClassName (VoxalysPhonationClass cls) noexcept {
	switch (cls) {
		case Modal:    return U"modal";
		case Breathy:  return U"breathy";
		case Pressed:  return U"pressed";
		case Rough:    return U"rough";
		case Unvoiced:
		default:       return U"unvoiced";
	}
}

std::vector<float> extractMonoFloatSamples (Sound me) {
	const integer n = my nx;
	std::vector<float> mono (static_cast<size_t> (n), 0.0f);
	if (my ny == 1) {
		for (integer i = 1; i <= n; i ++)
			mono [static_cast<size_t> (i - 1)] = static_cast<float> (my z [1] [i]);
	} else {
		const double invCh = 1.0 / static_cast<double> (my ny);
		for (integer i = 1; i <= n; i ++) {
			double sum = 0.0;
			for (integer ch = 1; ch <= my ny; ch ++)
				sum += my z [ch] [i];
			mono [static_cast<size_t> (i - 1)] = static_cast<float> (sum * invCh);
		}
	}
	return mono;
}

conststring32 thresholdFlag (double value, double threshold, conststring32 unitStr) {
	if (isundef (value))
		return U"N/A";
	if (value > threshold)
		return Melder_cat (U"ABOVE THRESHOLD (> ", Melder_fixed (threshold, 3), U" ", unitStr, U")");
	return Melder_cat (U"OK (<= ", Melder_fixed (threshold, 3), U" ", unitStr, U")");
}

MelderColour vrpProgressColour (double fraction) noexcept {
	if (fraction <= 0.2)
		return Melder_BLUE;
	if (fraction <= 0.4)
		return Melder_CYAN;
	if (fraction <= 0.6)
		return Melder_GREEN;
	if (fraction <= 0.8)
		return Melder_MAGENTA;
	return Melder_RED;
}

autoTable buildVrpTableFromPoints (const std::vector<VoxalysVrpPoint> &points) {
	const conststring32 columnNames [] = {
		U"time_s", U"f0_Hz", U"f0_st_re100Hz", U"spl_dB", U"voiced", U"quality", U"phonation"
	};
	const integer nrows = static_cast<integer> (points.size());
	autoTable table = Table_createWithColumnNames (nrows, ARRAY_TO_STRVEC (columnNames));
	for (integer row = 1; row <= nrows; row ++) {
		const VoxalysVrpPoint &pt = points [static_cast<size_t> (row - 1)];
		Table_setNumericValue (table.get(), row, 1, pt.time_sec);
		if (pt.voiced && pt.f0_hz > 0.0f) {
			Table_setNumericValue (table.get(), row, 2, static_cast<double> (pt.f0_hz));
			Table_setNumericValue (table.get(), row, 3, 12.0 * std::log2 (static_cast<double> (pt.f0_hz) / 100.0));
		} else {
			Table_setNumericValue (table.get(), row, 2, undefined);
			Table_setNumericValue (table.get(), row, 3, undefined);
		}
		Table_setNumericValue (table.get(), row, 4, static_cast<double> (pt.spl_db));
		Table_setNumericValue (table.get(), row, 5, pt.voiced ? 1.0 : 0.0);
		Table_setNumericValue (table.get(), row, 6, static_cast<double> (pt.quality_score));
		Table_setStringValue (table.get(), row, 7, phonationClassName (pt.phonation_class));
	}
	return table;
}

} // namespace

void Voxalys_analyzeSound (Sound me, double pitchFloor, double pitchCeiling, double splCalibrationOffset_dB) {
	try {
		Melder_require (pitchFloor >= 20.0 && pitchCeiling > pitchFloor,
			U"Voxalys: pitch floor must be >= 20 Hz and strictly below pitch ceiling.");

		const double sampleRate = 1.0 / my dx;
		const double duration = my xmax - my xmin;

		/* 1. Native Praat cross-correlation pitch, glottal pulses, harmonicity & LTAS. */
		autoPitch pitch = Sound_to_Pitch_rawCc (
			me, 0.0, pitchFloor, pitchCeiling,
			15, false, 0.03, 0.45, 0.01, 0.35, 0.14
		);
		autoPointProcess pulses = Sound_Pitch_to_PointProcess_cc (me, pitch.get());
		autoHarmonicity harm = Sound_to_Harmonicity_cc (me, 0.01, pitchFloor, 0.1, 1.0);
		autoLtas ltas = Sound_to_Ltas (me, 100.0);

		const double f0MeanHz = Pitch_getMean (pitch.get(), 0.0, 0.0, kPitch_unit::HERTZ);
		const double f0MedianHz = Pitch_getQuantile (pitch.get(), 0.0, 0.0, 0.5, kPitch_unit::HERTZ);
		const double f0SdHz = Pitch_getStandardDeviation (pitch.get(), 0.0, 0.0, kPitch_unit::HERTZ);
		const double f0MeanSt = Pitch_getMean (pitch.get(), 0.0, 0.0, kPitch_unit::SEMITONES_100);
		const double f0SdSt = Pitch_getStandardDeviation (pitch.get(), 0.0, 0.0, kPitch_unit::SEMITONES_100);

		const double minPeriod = 0.8 / pitchCeiling;
		const double maxPeriod = 1.25 / pitchFloor;
		const double maxPeriodFactor = 1.3;
		const double maxAmpFactor = 1.6;

		const double jitLocal = PointProcess_getJitter_local (pulses.get(), 0.0, 0.0, minPeriod, maxPeriod, maxPeriodFactor) * 100.0;
		const double jitAbsUs = PointProcess_getJitter_local_absolute (pulses.get(), 0.0, 0.0, minPeriod, maxPeriod, maxPeriodFactor) * 1e6;
		const double jitRap = PointProcess_getJitter_rap (pulses.get(), 0.0, 0.0, minPeriod, maxPeriod, maxPeriodFactor) * 100.0;
		const double jitPpq5 = PointProcess_getJitter_ppq5 (pulses.get(), 0.0, 0.0, minPeriod, maxPeriod, maxPeriodFactor) * 100.0;
		const double jitDdp = PointProcess_getJitter_ddp (pulses.get(), 0.0, 0.0, minPeriod, maxPeriod, maxPeriodFactor) * 100.0;

		double shimLocal = undefined, shimDb = undefined, shimApq3 = undefined;
		double shimApq5 = undefined, shimApq11 = undefined, shimDda = undefined;
		PointProcess_Sound_getShimmer_multi (
			pulses.get(), me, 0.0, 0.0, minPeriod, maxPeriod, maxPeriodFactor, maxAmpFactor,
			& shimLocal, & shimDb, & shimApq3, & shimApq5, & shimApq11, & shimDda
		);
		const double shimLocalPct = isdefined (shimLocal) ? shimLocal * 100.0 : undefined;
		const double shimApq3Pct = isdefined (shimApq3) ? shimApq3 * 100.0 : undefined;
		const double shimApq5Pct = isdefined (shimApq5) ? shimApq5 * 100.0 : undefined;
		const double shimApq11Pct = isdefined (shimApq11) ? shimApq11 * 100.0 : undefined;
		const double shimDdaPct = isdefined (shimDda) ? shimDda * 100.0 : undefined;

		const double hnrMeanDb = Harmonicity_getMean (harm.get(), 0.0, 0.0);
		const double ltasAlphaRatio = Ltas_getSlope (ltas.get(), 0.0, 1000.0, 1000.0, 4000.0, 1);

		/* 2. JoVoice Rust DSP / Model Engine metrics. */
		ScopedModelHandle model (voxalys_model_new (
			static_cast<float> (pitchFloor),
			static_cast<float> (pitchCeiling),
			static_cast<float> (splCalibrationOffset_dB)
		));
		if (! model.ptr)
			Melder_throw (U"Voxalys Rust engine initialization failed: ", Melder_peek8to32 (voxalys_last_error ()));

		const std::vector<float> mono = extractMonoFloatSamples (me);
		VoxalysSummaryResult summary {};
		const int32_t rc = voxalys_analyze_buffer (
			model.ptr, mono.data(), mono.size(),
			static_cast<float> (sampleRate), & summary
		);
		if (rc != 0)
			Melder_throw (U"Voxalys Rust analysis failed: ", Melder_peek8to32 (voxalys_last_error ()));

		/* 3. Emit formatted clinical report into Praat's Info window. */
		MelderInfo_open ();
		MelderInfo_writeLine (U"=== VOXALYS NATIVE VOICE & JOVOICE REPORT ===");
		MelderInfo_writeLine (U"Sound: ", my name.get(), U" | Duration: ", Melder_fixed (duration, 3), U" s | Fs: ", Melder_fixed (sampleRate, 0), U" Hz");
		MelderInfo_writeLine (U"Engine: ", Melder_peek8to32 (voxalys_version ()));
		MelderInfo_writeLine (U"");
		MelderInfo_writeLine (U"--- Praat Native Fundamental Frequency (CC) ---");
		MelderInfo_writeLine (U"Mean F0:   ", Melder_fixed (f0MeanHz, 2), U" Hz (", Melder_fixed (f0MeanSt, 2), U" st re 100 Hz)");
		MelderInfo_writeLine (U"Median F0: ", Melder_fixed (f0MedianHz, 2), U" Hz");
		MelderInfo_writeLine (U"SD F0:     ", Melder_fixed (f0SdHz, 2), U" Hz (", Melder_fixed (f0SdSt, 2), U" st)");
		MelderInfo_writeLine (U"");
		MelderInfo_writeLine (U"--- Perturbation (Deliyski 1993 / MDVP Thresholds) ---");
		MelderInfo_writeLine (U"Jitter (local):      ", Melder_fixed (jitLocal, 3), U" %   [", thresholdFlag (jitLocal, 1.040, U"%"), U"]");
		MelderInfo_writeLine (U"Jitter (local, abs): ", Melder_fixed (jitAbsUs, 2), U" us  [", thresholdFlag (jitAbsUs, 83.200, U"us"), U"]");
		MelderInfo_writeLine (U"Jitter (RAP):        ", Melder_fixed (jitRap, 3), U" %   [", thresholdFlag (jitRap, 0.680, U"%"), U"]");
		MelderInfo_writeLine (U"Jitter (PPQ5):       ", Melder_fixed (jitPpq5, 3), U" %   [", thresholdFlag (jitPpq5, 0.840, U"%"), U"]");
		MelderInfo_writeLine (U"Jitter (DDP):        ", Melder_fixed (jitDdp, 3), U" %");
		MelderInfo_writeLine (U"Shimmer (local):     ", Melder_fixed (shimLocalPct, 3), U" %   [", thresholdFlag (shimLocalPct, 3.810, U"%"), U"]");
		MelderInfo_writeLine (U"Shimmer (local, dB): ", Melder_fixed (shimDb, 3), U" dB  [", thresholdFlag (shimDb, 0.350, U"dB"), U"]");
		MelderInfo_writeLine (U"Shimmer (APQ3):      ", Melder_fixed (shimApq3Pct, 3), U" %   [", thresholdFlag (shimApq3Pct, 3.070, U"%"), U"]");
		MelderInfo_writeLine (U"Shimmer (APQ5):      ", Melder_fixed (shimApq5Pct, 3), U" %");
		MelderInfo_writeLine (U"Shimmer (APQ11):     ", Melder_fixed (shimApq11Pct, 3), U" %");
		MelderInfo_writeLine (U"Shimmer (DDA):       ", Melder_fixed (shimDdaPct, 3), U" %");
		MelderInfo_writeLine (U"");
		MelderInfo_writeLine (U"--- Harmonicity & LTAS ---");
		MelderInfo_writeLine (U"Mean HNR (cc):             ", Melder_fixed (hnrMeanDb, 2), U" dB");
		MelderInfo_writeLine (U"LTAS Slope (0-1k / 1k-4k): ", Melder_fixed (ltasAlphaRatio, 2), U" dB");
		MelderInfo_writeLine (U"");
		MelderInfo_writeLine (U"--- JoVoice Rust Core Metrics ---");
		MelderInfo_writeLine (U"Frames analyzed:    ", static_cast<integer> (summary.frame_count),
			U" (voiced: ", static_cast<integer> (summary.voiced_frame_count),
			U", ", Melder_fixed (summary.voiced_fraction * 100.0, 1), U" %)");
		MelderInfo_writeLine (U"JoVoice Mean F0:    ", Melder_fixed (summary.f0_mean_hz, 2), U" Hz (median ", Melder_fixed (summary.f0_median_hz, 2), U" Hz, SD ", Melder_fixed (summary.f0_sd_hz, 2), U" Hz)");
		MelderInfo_writeLine (U"JoVoice F0 Range:   ", Melder_fixed (summary.f0_min_hz, 1), U" - ", Melder_fixed (summary.f0_max_hz, 1), U" Hz (", Melder_fixed (summary.f0_range_semitones, 2), U" st)");
		MelderInfo_writeLine (U"JoVoice SPL:        ", Melder_fixed (summary.spl_mean_db, 1), U" dB (range ", Melder_fixed (summary.spl_min_db, 1), U" - ", Melder_fixed (summary.spl_max_db, 1), U" dB, DR ", Melder_fixed (summary.spl_dynamic_range_db, 1), U" dB)");
		MelderInfo_writeLine (U"JoVoice Confidence: ", Melder_fixed (summary.mean_confidence, 3), U" | Quality score: ", Melder_fixed (summary.mean_quality_score, 3));
		MelderInfo_writeLine (U"VRP Area (st*dB):   ", Melder_fixed (summary.vrp_area_st_db, 1));
		MelderInfo_close ();
	} catch (MelderError) {
		Melder_throw (me, U": Voxalys voice analysis not completed.");
	}
}

autoTable Voxalys_Sound_to_VRPTable (Sound me, double pitchFloor, double pitchCeiling, double splCalibrationOffset_dB) {
	try {
		Melder_require (pitchFloor >= 20.0 && pitchCeiling > pitchFloor,
			U"Voxalys: pitch floor must be >= 20 Hz and strictly below pitch ceiling.");

		const double sampleRate = 1.0 / my dx;
		const std::vector<float> mono = extractMonoFloatSamples (me);

		ScopedStreamHandle stream (voxalys_stream_new (
			static_cast<float> (sampleRate),
			65536,
			static_cast<float> (pitchFloor),
			static_cast<float> (pitchCeiling),
			static_cast<float> (splCalibrationOffset_dB)
		));
		if (! stream.ptr)
			Melder_throw (U"Voxalys Rust stream initialization failed: ", Melder_peek8to32 (voxalys_last_error ()));

		std::vector<VoxalysVrpPoint> allPoints;
		allPoints.reserve (mono.size() / 400 + 32);
		VoxalysVrpPoint pollBuf [128];

		constexpr size_t chunkSize = 512;
		size_t offset = 0;
		while (offset < mono.size()) {
			const size_t n = std::min (chunkSize, mono.size() - offset);
			const size_t pushed = voxalys_stream_push_samples (stream.ptr, mono.data() + offset, n);
			offset += pushed;

			for (;;) {
				const size_t got = voxalys_stream_poll_points (stream.ptr, pollBuf, 128);
				if (got == 0)
					break;
				allPoints.insert (allPoints.end(), pollBuf, pollBuf + got);
			}
			if (pushed == 0)
				break;
		}

		/* Final drain of any remaining analyzed points. */
		for (;;) {
			const size_t got = voxalys_stream_poll_points (stream.ptr, pollBuf, 128);
			if (got == 0)
				break;
			allPoints.insert (allPoints.end(), pollBuf, pollBuf + got);
		}

		return buildVrpTableFromPoints (allPoints);
	} catch (MelderError) {
		Melder_throw (me, U": Voxalys VRP trajectory Table not computed.");
	}
}

/* =========================================================================
 * Interactive Real-Time PortAudio VRP Editor (VoxalysVrpRecorder)
 * ========================================================================= */

static int vrpPortAudioCallback (
	const void *input, void * /* output */,
	unsigned long frameCount,
	const PaStreamCallbackTimeInfo * /* timeInfo */,
	PaStreamCallbackFlags /* statusFlags */,
	void *userData) noexcept
{
	VoxalysVrpRecorder me = static_cast<VoxalysVrpRecorder> (userData);
	if (! me || ! input || frameCount == 0)
		return paContinue;

	const float *samples = static_cast<const float *> (input);

	/* 1. Push into lock-free SPSC Rust ring buffer (zero allocs, zero locks). */
	if (my rustStream)
		voxalys_stream_push_samples (my rustStream, samples, static_cast<size_t> (frameCount));

	/* 2. Append to pre-allocated uninterrupted recording buffer. */
	const size_t cap = my recordBuffer.size();
	const size_t cur = my recordedSamples.load (std::memory_order_relaxed);
	if (cur < cap) {
		const size_t toCopy = std::min (static_cast<size_t> (frameCount), cap - cur);
		std::copy (samples, samples + toCopy, my recordBuffer.data() + cur);
		my recordedSamples.store (cur + toCopy, std::memory_order_release);
		if (cur + toCopy >= cap)
			return paComplete;
	} else {
		return paComplete;
	}
	return paContinue;
}

static void stopAudioStream (VoxalysVrpRecorder me) noexcept {
	my recording = false;
	if (my portaudioStream) {
		Pa_StopStream (my portaudioStream);
		Pa_CloseStream (my portaudioStream);
		my portaudioStream = nullptr;
	}
}

static double hzToSemitones (double hz) noexcept {
	return 12.0 * std::log2 (hz / 100.0);
}

static double midiToSemitones (int midi) noexcept {
	const double hz = 440.0 * std::pow (2.0, static_cast<double> (midi - 69) / 12.0);
	return hzToSemitones (hz);
}

static int hzToMidi (double hz) noexcept {
	return static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (hz / 440.0)));
}

static bool isBlackPianoKey (int midi) noexcept {
	const int pc = ((midi % 12) + 12) % 12;
	return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

static conststring32 midiNoteName (int midi) {
	static constexpr conststring32 names [12] = {
		U"C", U"C#", U"D", U"D#", U"E", U"F", U"F#", U"G", U"G#", U"A", U"A#", U"B"
	};
	const int pc = ((midi % 12) + 12) % 12;
	const int octave = (midi >= 0 ? midi / 12 : (midi - 11) / 12) - 1;
	return Melder_cat (names [pc], static_cast<integer> (octave));
}

static void drawPianoKeyboardStrip (Graphics g, double viewF0Min, double viewF0Max, double xMin, double xMax, int liveMidi) {
	Graphics_setWindow (g, xMin, xMax, 0.0, 1.0);
	const int minMidi = static_cast<int> (std::floor (69.0 + 12.0 * std::log2 (viewF0Min / 440.0))) - 2;
	const int maxMidi = static_cast<int> (std::ceil (69.0 + 12.0 * std::log2 (viewF0Max / 440.0))) + 2;

	/* Pass 1: White keys spanning midpoints between adjacent white notes. */
	Graphics_setLineWidth (g, 1.0);
	Graphics_setFontSize (g, 8.0);
	Graphics_setTextAlignment (g, Graphics_CENTRE, Graphics_BOTTOM);
	for (int m = minMidi; m <= maxMidi; m ++) {
		if (isBlackPianoKey (m))
			continue;
		const int prevW = isBlackPianoKey (m - 1) ? m - 2 : m - 1;
		const int nextW = isBlackPianoKey (m + 1) ? m + 2 : m + 1;
		const double rawL = 0.5 * (midiToSemitones (prevW) + midiToSemitones (m));
		const double rawR = 0.5 * (midiToSemitones (m) + midiToSemitones (nextW));
		const double xL = std::max (rawL, xMin);
		const double xR = std::min (rawR, xMax);
		if (xR <= xL)
			continue;

		Graphics_setColour (g, m == liveMidi ? Melder_CYAN : Melder_WHITE);
		Graphics_fillRectangle (g, xL, xR, 0.0, 1.0);
		Graphics_setColour (g, Melder_BLACK);
		Graphics_rectangle (g, xL, xR, 0.0, 1.0);

		/* Label every C octave (C2, C3, C4, C5, C6) and A2/A3/A4 inside white keys. */
		const int pc = ((m % 12) + 12) % 12;
		if (pc == 0 && (xR - xL) >= 0.45)
			Graphics_text (g, 0.5 * (xL + xR), 0.06, midiNoteName (m));
	}

	/* Pass 2: Black keys centered on sharp/flat semitones, upper 62% of keyboard height. */
	for (int m = minMidi; m <= maxMidi; m ++) {
		if (! isBlackPianoKey (m))
			continue;
		const double cx = midiToSemitones (m);
		const double xL = std::max (cx - 0.34, xMin);
		const double xR = std::min (cx + 0.34, xMax);
		if (xR <= xL)
			continue;
		Graphics_setColour (g, m == liveMidi ? Melder_RED : Melder_BLACK);
		Graphics_fillRectangle (g, xL, xR, 0.38, 1.0);
		Graphics_setColour (g, Melder_BLACK);
		Graphics_rectangle (g, xL, xR, 0.38, 1.0);
	}

	/* Outer border and Hz frequency ticks right underneath the piano keyboard. */
	Graphics_setColour (g, Melder_BLACK);
	Graphics_setLineWidth (g, 1.0);
	Graphics_rectangle (g, xMin, xMax, 0.0, 1.0);

	Graphics_setFontSize (g, 9.0);
	static constexpr double hzTicks [] = { 55.0, 65.41, 82.41, 110.0, 130.81, 164.81, 220.0, 261.63, 329.63, 440.0, 523.25, 659.25, 880.0, 1046.50 };
	for (double hz : hzTicks) {
		if (hz >= viewF0Min && hz <= viewF0Max)
			Graphics_markBottom (g, hzToSemitones (hz), false, true, false, Melder_fixed (hz, 0));
	}
	Graphics_textBottom (g, true, U"Fundamental Frequency F0 (Piano Keys / Hz, semitone scale)");
}

static void drawVrpCanvas (VoxalysVrpRecorder me) {
	if (! my graphics)
		return;
	Graphics g = my graphics.get();
	Graphics_clearWs (g);

	double outerX1 = 0.0, outerX2 = 0.0, outerY1 = 0.0, outerY2 = 0.0;
	Graphics_inqViewport (g, & outerX1, & outerX2, & outerY1, & outerY2);

	Graphics_setInner (g);
	double ix1 = 0.0, ix2 = 0.0, iy1 = 0.0, iy2 = 0.0;
	Graphics_inqViewport (g, & ix1, & ix2, & iy1, & iy2);
	Graphics_unsetInner (g);

	const double xMin = my useSemitoneScale ? hzToSemitones (my viewF0Min) : my viewF0Min;
	const double xMax = my useSemitoneScale ? hzToSemitones (my viewF0Max) : my viewF0Max;
	const int liveMidi = (my hasLatestPoint && my latestPoint.voiced && my latestPoint.f0_hz > 0.0f)
		? hzToMidi (static_cast<double> (my latestPoint.f0_hz))
		: -1;

	/* Reserve bottom 16% of inner area for the aligned piano keyboard in semitone mode. */
	const double plotBottomY = my useSemitoneScale ? (iy1 + 0.16 * (iy2 - iy1)) : iy1;

	if (my useSemitoneScale) {
		Graphics_setViewport (g, ix1, ix2, iy1, plotBottomY);
		drawPianoKeyboardStrip (g, my viewF0Min, my viewF0Max, xMin, xMax, liveMidi);
	}

	/* Main VRP scatter & envelope viewport. */
	Graphics_setViewport (g, ix1, ix2, plotBottomY, iy2);
	Graphics_setWindow (g, xMin, xMax, my splFloor, my splCeiling);

	Graphics_setColour (g, Melder_BLACK);
	Graphics_setLineWidth (g, 1.0);
	Graphics_setFontSize (g, 10.0);
	Graphics_drawInnerBox (g);

	if (my useSemitoneScale) {
		/* Subtle vertical octave reference lines aligned with C notes on the piano below. */
		static constexpr double cNotes [] = { 65.41, 130.81, 261.63, 523.25, 1046.50 };
		Graphics_setLineType (g, Graphics_DOTTED);
		Graphics_setGrey (g, 0.72);
		for (double hz : cNotes) {
			if (hz > my viewF0Min && hz < my viewF0Max) {
				const double cx = hzToSemitones (hz);
				Graphics_line (g, cx, my splFloor, cx, my splCeiling);
			}
		}
		Graphics_setLineType (g, Graphics_DRAWN);
		Graphics_setColour (g, Melder_BLACK);
	} else {
		const double span = my viewF0Max - my viewF0Min;
		const double stepHz = span <= 250.0 ? 25.0 : (span <= 550.0 ? 50.0 : 100.0);
		Graphics_marksBottomEvery (g, 1.0, stepHz, true, true, true);
		Graphics_textBottom (g, true, U"Fundamental Frequency F0 (Hz, linear)");
	}

	const double splSpan = my splCeiling - my splFloor;
	const double splStep = splSpan <= 40.0 ? 5.0 : 10.0;
	Graphics_marksLeftEvery (g, 1.0, splStep, true, true, true);
	Graphics_textLeft (g, true, U"Intensity / SPL (dB)");
	Graphics_textTop (g, false, Melder_cat (
		U"Voxalys Interactive VRP [", Melder_fixed (my viewF0Min, 0), U"-", Melder_fixed (my viewF0Max, 0),
		U" Hz | ", Melder_fixed (my splFloor, 0), U"-", Melder_fixed (my splCeiling, 0),
		U" dB] (Blue = Start -> Red = End)"
	));

	auto mapX = [me](double hz) noexcept -> double {
		return my useSemitoneScale ? hzToSemitones (hz) : hz;
	};

	/* Draw voiced VRP points with chronological color progression and compute envelope. */
	std::map<int, std::pair<double, double>> envBySemitone;
	const size_t total = my points.size();
	for (size_t idx = 0; idx < total; idx ++) {
		const VoxalysVrpPoint &pt = my points [idx];
		if (! pt.voiced || pt.f0_hz < my viewF0Min || pt.f0_hz > my viewF0Max)
			continue;
		if (pt.spl_db < my splFloor || pt.spl_db > my splCeiling)
			continue;

		const double frac = total > 1 ? static_cast<double> (idx) / static_cast<double> (total - 1) : 0.0;
		Graphics_setColour (g, vrpProgressColour (frac));
		Graphics_fillCircle_mm (g, mapX (static_cast<double> (pt.f0_hz)), static_cast<double> (pt.spl_db), 2.2);

		const int stBin = static_cast<int> (std::lround (hzToSemitones (static_cast<double> (pt.f0_hz))));
		auto it = envBySemitone.find (stBin);
		if (it == envBySemitone.end()) {
			envBySemitone [stBin] = { static_cast<double> (pt.spl_db), static_cast<double> (pt.spl_db) };
		} else {
			it -> second.first = std::min (it -> second.first, static_cast<double> (pt.spl_db));
			it -> second.second = std::max (it -> second.second, static_cast<double> (pt.spl_db));
		}
	}

	/* Draw upper and lower VRP envelope contour lines across occupied semitone bins. */
	if (envBySemitone.size() >= 2) {
		Graphics_setColour (g, Melder_BLACK);
		Graphics_setLineWidth (g, 1.5);
		bool hasPrev = false;
		double prevX = 0.0, prevMin = 0.0, prevMax = 0.0;
		for (const auto &kv : envBySemitone) {
			const double binF0 = 100.0 * std::pow (2.0, static_cast<double> (kv.first) / 12.0);
			if (binF0 < my viewF0Min || binF0 > my viewF0Max)
				continue;
			const double curX = mapX (binF0);
			if (hasPrev) {
				Graphics_line (g, prevX, prevMin, curX, kv.second.first);
				Graphics_line (g, prevX, prevMax, curX, kv.second.second);
			}
			prevX = curX;
			prevMin = kv.second.first;
			prevMax = kv.second.second;
			hasPrev = true;
		}
	}

	/* Highlight the live cursor point. */
	if (my hasLatestPoint && my latestPoint.voiced
			&& my latestPoint.f0_hz >= my viewF0Min && my latestPoint.f0_hz <= my viewF0Max
			&& my latestPoint.spl_db >= my splFloor && my latestPoint.spl_db <= my splCeiling) {
		Graphics_setColour (g, Melder_RED);
		Graphics_setLineWidth (g, 2.0);
		Graphics_circle_mm (g, mapX (static_cast<double> (my latestPoint.f0_hz)), static_cast<double> (my latestPoint.spl_db), 4.0);
	}

	/* Restore outer viewport. */
	Graphics_setViewport (g, outerX1, outerX2, outerY1, outerY2);
}

static void updateStatusLabel (VoxalysVrpRecorder me) {
	if (! my statusLabel)
		return;
	const double durSec = static_cast<double> (my recordedSamples.load (std::memory_order_acquire)) / my sampleRate;
	size_t voicedCount = 0;
	for (const auto &pt : my points)
		if (pt.voiced)
			voicedCount ++;

	if (my hasLatestPoint && my latestPoint.voiced) {
		const int liveNote = hzToMidi (static_cast<double> (my latestPoint.f0_hz));
		GuiLabel_setText (my statusLabel, Melder_cat (
			my recording ? U"[REC] " : (my paused ? U"[PAUSED] " : U"[IDLE] "),
			U"F0: ", Melder_fixed (my latestPoint.f0_hz, 1), U" Hz (", midiNoteName (liveNote), U") | ",
			U"SPL: ", Melder_fixed (my latestPoint.spl_db, 1), U" dB | ",
			U"Qual: ", Melder_fixed (my latestPoint.quality_score, 2),
			U" (", phonationClassName (my latestPoint.phonation_class), U") | ",
			U"Pts: ", static_cast<integer> (voicedCount),
			U" | Dur: ", Melder_fixed (durSec, 1), U" s"
		));
	} else {
		GuiLabel_setText (my statusLabel, Melder_cat (
			my recording ? U"[REC] " : (my paused ? U"[PAUSED] " : U"[IDLE] "),
			U"Waiting for voiced phonation... | ",
			U"Pts: ", static_cast<integer> (voicedCount),
			U" | Dur: ", Melder_fixed (durSec, 1), U" s"
		));
	}
}

#if cocoa
static void vrpTimerCallback (CFRunLoopTimerRef /* timer */, void *void_me)
#elif gtk
static gboolean vrpTimerCallback (void *void_me)
#else
static bool vrpTimerCallback (void *void_me)
#endif
{
	VoxalysVrpRecorder me = static_cast<VoxalysVrpRecorder> (void_me);
	try {
		if (me && my rustStream) {
			VoxalysVrpPoint pollBuf [64];
			bool gotNew = false;
			for (;;) {
				const size_t n = voxalys_stream_poll_points (my rustStream, pollBuf, 64);
				if (n == 0)
					break;
				my points.insert (my points.end(), pollBuf, pollBuf + n);
				my latestPoint = pollBuf [n - 1];
				my hasLatestPoint = true;
				gotNew = true;
			}
			if (gotNew || my recording) {
				updateStatusLabel (me);
				if (my graphics)
					Graphics_updateWs (my graphics.get());
			}
		}
	} catch (MelderError) {
		Melder_flushError ();
	}
	#if cocoa
		return;
	#elif gtk
		return TRUE;
	#else
		return false;
	#endif
}

static void gui_drawingarea_cb_expose (VoxalysVrpRecorder me, GuiDrawingArea_ExposeEvent /* event */) {
	drawVrpCanvas (me);
}

static void gui_drawingarea_cb_resize (VoxalysVrpRecorder me, GuiDrawingArea_ResizeEvent event) {
	if (! my graphics)
		return;
	Graphics_setWsViewport (my graphics.get(), 0, event -> width, 0, event -> height);
	Graphics_setWsWindow (my graphics.get(), 0, event -> width, 0, event -> height);
	Graphics_setViewport (my graphics.get(), 0, event -> width, 0, event -> height);
	Graphics_updateWs (my graphics.get());
}

static void startAudioStream (VoxalysVrpRecorder me) {
	if (my recording)
		return;
	PaStreamParameters inputParams {};
	inputParams. device = my selectedDevice;
	inputParams. channelCount = 1;
	inputParams. sampleFormat = paFloat32;
	const PaDeviceInfo *info = Pa_GetDeviceInfo (my selectedDevice);
	if (! info)
		Melder_throw (U"Selected audio input device is unavailable.");
	inputParams. suggestedLatency = info -> defaultLowInputLatency;

	const PaError openErr = Pa_OpenStream (
		& my portaudioStream,
		& inputParams,
		nullptr,
		my sampleRate,
		256,
		paClipOff,
		vrpPortAudioCallback,
		static_cast<void *> (me)
	);
	if (openErr != paNoError)
		Melder_throw (U"Cannot open PortAudio input stream: ", Melder_peek8to32 (Pa_GetErrorText (openErr)));

	const PaError startErr = Pa_StartStream (my portaudioStream);
	if (startErr != paNoError) {
		Pa_CloseStream (my portaudioStream);
		my portaudioStream = nullptr;
		Melder_throw (U"Cannot start PortAudio input stream: ", Melder_peek8to32 (Pa_GetErrorText (startErr)));
	}
	my recording = true;
	my paused = false;
}

static void gui_button_cb_start (VoxalysVrpRecorder me, GuiButtonEvent /* event */) {
	try {
		startAudioStream (me);
		if (my pauseButton)
			GuiButton_setText (my pauseButton, U"Pause");
		updateStatusLabel (me);
	} catch (MelderError) {
		Melder_flushError (U"Could not start VRP recording.");
	}
}

static void gui_button_cb_pause (VoxalysVrpRecorder me, GuiButtonEvent /* event */) {
	try {
		if (my recording) {
			stopAudioStream (me);
			my paused = true;
			if (my pauseButton)
				GuiButton_setText (my pauseButton, U"Resume");
		} else if (my paused) {
			startAudioStream (me);
			if (my pauseButton)
				GuiButton_setText (my pauseButton, U"Pause");
		}
		updateStatusLabel (me);
	} catch (MelderError) {
		Melder_flushError (U"Could not toggle pause/resume.");
	}
}

static void gui_button_cb_clear (VoxalysVrpRecorder me, GuiButtonEvent /* event */) {
	stopAudioStream (me);
	my paused = false;
	my recordedSamples.store (0, std::memory_order_release);
	my points.clear ();
	my hasLatestPoint = false;
	if (my rustStream)
		voxalys_stream_reset (my rustStream);
	if (my pauseButton)
		GuiButton_setText (my pauseButton, U"Pause");
	updateStatusLabel (me);
	if (my graphics)
		Graphics_updateWs (my graphics.get());
}

static void gui_button_cb_save (VoxalysVrpRecorder me, GuiButtonEvent /* event */) {
	try {
		stopAudioStream (me);
		my paused = false;
		if (my pauseButton)
			GuiButton_setText (my pauseButton, U"Pause");

		const size_t nsamp = my recordedSamples.load (std::memory_order_acquire);
		if (nsamp == 0 && my points.empty())
			Melder_throw (U"Nothing recorded yet. Click Start to record a voice range profile.");

		autostring32 baseName = my recordingNameField
			? GuiText_getString (my recordingNameField)
			: Melder_dup (U"Voxalys_VRP");
		if (! baseName || baseName [0] == U'\0')
			baseName = Melder_dup (U"Voxalys_VRP");

		if (nsamp > 0) {
			autoSound sound = Sound_createSimple (1, static_cast<double> (nsamp) / my sampleRate, my sampleRate);
			for (size_t i = 0; i < nsamp; i ++)
				sound -> z [1] [static_cast<integer> (i + 1)] = static_cast<double> (my recordBuffer [i]);
			Thing_setName (sound.get(), baseName.get());
			Editor_broadcastPublication (me, sound.move());
		}

		if (! my points.empty()) {
			autoTable table = buildVrpTableFromPoints (my points);
			Thing_setName (table.get(), Melder_cat (baseName.get(), U"_VRP"));
			Editor_broadcastPublication (me, table.move());
		}
		updateStatusLabel (me);
	} catch (MelderError) {
		Melder_flushError (U"Could not publish VRP Sound and Table to Objects list.");
	}
}

static void setDisplayRanges (VoxalysVrpRecorder me, double f0Min, double f0Max, double splMin, double splMax) {
	if (f0Min < 20.0 || f0Max <= f0Min || splMax <= splMin)
		return;
	my viewF0Min = f0Min;
	my viewF0Max = f0Max;
	my splFloor = splMin;
	my splCeiling = splMax;
	if (my graphics)
		Graphics_updateWs (my graphics.get());
}

static void autoFitToPoints (VoxalysVrpRecorder me) {
	double minF0 = 1e9, maxF0 = 0.0;
	double minSpl = 1e9, maxSpl = -1e9;
	size_t count = 0;
	for (const auto &pt : my points) {
		if (! pt.voiced || pt.f0_hz <= 0.0f)
			continue;
		minF0 = std::min (minF0, static_cast<double> (pt.f0_hz));
		maxF0 = std::max (maxF0, static_cast<double> (pt.f0_hz));
		minSpl = std::min (minSpl, static_cast<double> (pt.spl_db));
		maxSpl = std::max (maxSpl, static_cast<double> (pt.spl_db));
		count ++;
	}
	if (count == 0)
		return;
	/* Add ~2 semitones padding on pitch and +/- 6 dB on SPL. */
	const double padF0Min = std::max (40.0, std::floor (minF0 / std::pow (2.0, 2.0 / 12.0)));
	const double padF0Max = std::min (1200.0, std::ceil (maxF0 * std::pow (2.0, 2.0 / 12.0)));
	const double padSplMin = std::floor ((minSpl - 6.0) / 5.0) * 5.0;
	const double padSplMax = std::ceil ((maxSpl + 6.0) / 5.0) * 5.0;
	setDisplayRanges (me, padF0Min, std::max (padF0Max, padF0Min + 30.0),
		padSplMin, std::max (padSplMax, padSplMin + 20.0));
}

static void gui_button_cb_bassPreset (VoxalysVrpRecorder me, GuiButtonEvent /* event */) {
	setDisplayRanges (me, 50.0, 450.0, my splFloor, my splCeiling);
}

static void gui_button_cb_fullPreset (VoxalysVrpRecorder me, GuiButtonEvent /* event */) {
	setDisplayRanges (me, 50.0, 1050.0, 40.0, 115.0);
}

static void gui_button_cb_autoFit (VoxalysVrpRecorder me, GuiButtonEvent /* event */) {
	autoFitToPoints (me);
}

static void gui_button_cb_toggleScale (VoxalysVrpRecorder me, GuiButtonEvent /* event */) {
	my useSemitoneScale = ! my useSemitoneScale;
	if (my scaleToggleButton)
		GuiButton_setText (my scaleToggleButton, my useSemitoneScale ? U"Scale: Log" : U"Scale: Hz");
	if (my graphics)
		Graphics_updateWs (my graphics.get());
}

static void menu_cb_displayRanges (VoxalysVrpRecorder me, EDITOR_ARGS) {
	EDITOR_FORM (U"Voxalys VRP: Display ranges", nullptr)
		POSITIVE (f0Min, U"F0 minimum (Hz)", U"50.0")
		POSITIVE (f0Max, U"F0 maximum (Hz)", U"450.0")
		REAL (splMin, U"SPL minimum (dB)", U"40.0")
		REAL (splMax, U"SPL maximum (dB)", U"115.0")
	EDITOR_OK
		SET_REAL (f0Min, my viewF0Min)
		SET_REAL (f0Max, my viewF0Max)
		SET_REAL (splMin, my splFloor)
		SET_REAL (splMax, my splCeiling)
	EDITOR_DO
		Melder_require (f0Min >= 20.0 && f0Max > f0Min,
			U"F0 minimum must be >= 20 Hz and strictly below F0 maximum.");
		Melder_require (splMax > splMin,
			U"SPL maximum must be greater than SPL minimum.");
		setDisplayRanges (me, f0Min, f0Max, splMin, splMax);
	EDITOR_END
}

static void menu_cb_autoFit (VoxalysVrpRecorder me, EDITOR_ARGS) {
	autoFitToPoints (me);
}

static void menu_cb_presetBass (VoxalysVrpRecorder me, EDITOR_ARGS) {
	setDisplayRanges (me, 50.0, 450.0, my splFloor, my splCeiling);
}

static void menu_cb_presetTenorAlto (VoxalysVrpRecorder me, EDITOR_ARGS) {
	setDisplayRanges (me, 75.0, 700.0, my splFloor, my splCeiling);
}

static void menu_cb_presetSoprano (VoxalysVrpRecorder me, EDITOR_ARGS) {
	setDisplayRanges (me, 130.0, 1100.0, my splFloor, my splCeiling);
}

static void menu_cb_presetFull (VoxalysVrpRecorder me, EDITOR_ARGS) {
	setDisplayRanges (me, 50.0, 1050.0, 40.0, 115.0);
}

static void menu_cb_toggleScale (VoxalysVrpRecorder me, EDITOR_ARGS) {
	gui_button_cb_toggleScale (me, nullptr);
}

static void gui_button_cb_close (VoxalysVrpRecorder me, GuiButtonEvent /* event */) {
	stopAudioStream (me);
	forget (me);
}

void structVoxalysVrpRecorder :: v9_destroy () noexcept {
	stopAudioStream (this);
	#if cocoa
		if (our d_cocoaTimer) {
			CFRunLoopTimerInvalidate (our d_cocoaTimer);
			our d_cocoaTimer = nullptr;
		}
	#elif gtk
		if (our gtkTimerId != 0) {
			g_source_remove (our gtkTimerId);
			our gtkTimerId = 0;
		}
	#elif motif
		if (our workProcId) {
			XtRemoveWorkProc (our workProcId);
			our workProcId = 0;
		}
	#endif
	if (our rustStream) {
		voxalys_stream_free (our rustStream);
		our rustStream = nullptr;
	}
	our VoxalysVrpRecorder_Parent :: v9_destroy ();
}

void structVoxalysVrpRecorder :: v_createChildren () {
	const integer topY = Machine_getMenuBarBottom () + 10;
	our statusLabel = GuiLabel_createShown (
		our windowForm, 16, -16, topY, topY + Gui_LABEL_HEIGHT + 4,
		U"[IDLE] Click Start to record Interactive Voice Range Profile", 0
	);

	const integer canvasTop = topY + Gui_LABEL_HEIGHT + 12;
	our canvas = GuiDrawingArea_createShown (
		our windowForm, 16, -16, canvasTop, -98,
		gui_drawingarea_cb_expose, nullptr, nullptr, gui_drawingarea_cb_resize, nullptr,
		this, GuiDrawingArea_BORDER
	);

	const integer row1Bottom = -56;
	GuiLabel_createShown (our windowForm, 16, 72, row1Bottom - Gui_TEXTFIELD_HEIGHT, row1Bottom, U"Name:", GuiLabel_RIGHT);
	our recordingNameField = GuiText_createShown (our windowForm, 78, 250, row1Bottom - Gui_TEXTFIELD_HEIGHT, row1Bottom, 0);
	GuiText_setString (our recordingNameField, U"Klient_01_VRP");

	our bassPresetButton = GuiButton_createShown (our windowForm, 266, 386, row1Bottom - Gui_PUSHBUTTON_HEIGHT, row1Bottom,
		U"Bass (50-450)", gui_button_cb_bassPreset, this, 0);
	our fullPresetButton = GuiButton_createShown (our windowForm, 394, 514, row1Bottom - Gui_PUSHBUTTON_HEIGHT, row1Bottom,
		U"Full (50-1050)", gui_button_cb_fullPreset, this, 0);
	our autoFitButton = GuiButton_createShown (our windowForm, 522, 622, row1Bottom - Gui_PUSHBUTTON_HEIGHT, row1Bottom,
		U"Auto-Fit", gui_button_cb_autoFit, this, 0);
	our scaleToggleButton = GuiButton_createShown (our windowForm, 630, -16, row1Bottom - Gui_PUSHBUTTON_HEIGHT, row1Bottom,
		U"Scale: Log", gui_button_cb_toggleScale, this, 0);

	const integer row2Bottom = -16;
	our startButton = GuiButton_createShown (our windowForm, 16, 106, row2Bottom - Gui_PUSHBUTTON_HEIGHT, row2Bottom,
		U"Start", gui_button_cb_start, this, 0);
	our pauseButton = GuiButton_createShown (our windowForm, 116, 216, row2Bottom - Gui_PUSHBUTTON_HEIGHT, row2Bottom,
		U"Pause", gui_button_cb_pause, this, 0);
	our clearButton = GuiButton_createShown (our windowForm, 226, 316, row2Bottom - Gui_PUSHBUTTON_HEIGHT, row2Bottom,
		U"Clear", gui_button_cb_clear, this, 0);
	our saveButton = GuiButton_createShown (our windowForm, -320, -110, row2Bottom - Gui_PUSHBUTTON_HEIGHT, row2Bottom,
		U"Save Sound & VRP to List", gui_button_cb_save, this, GuiButton_DEFAULT);
	our closeButton = GuiButton_createShown (our windowForm, -100, -16, row2Bottom - Gui_PUSHBUTTON_HEIGHT, row2Bottom,
		U"Close", gui_button_cb_close, this, 0);
}

void structVoxalysVrpRecorder :: v_createMenus () {
	VoxalysVrpRecorder_Parent :: v_createMenus ();
	Editor_addMenu (this, U"View", 0);
	Editor_addCommand (this, U"View", U"Display ranges...", 0, menu_cb_displayRanges);
	Editor_addCommand (this, U"View", U"Auto-fit to recorded points", 0, menu_cb_autoFit);
	Editor_addCommand (this, U"View", U"-- presets --", 0, nullptr);
	Editor_addCommand (this, U"View", U"Preset: Bass / Baritone (50-450 Hz)", 0, menu_cb_presetBass);
	Editor_addCommand (this, U"View", U"Preset: Tenor / Alto (75-700 Hz)", 0, menu_cb_presetTenorAlto);
	Editor_addCommand (this, U"View", U"Preset: Soprano (130-1100 Hz)", 0, menu_cb_presetSoprano);
	Editor_addCommand (this, U"View", U"Preset: Full Clinical (50-1050 Hz)", 0, menu_cb_presetFull);
	Editor_addCommand (this, U"View", U"-- scale --", 0, nullptr);
	Editor_addCommand (this, U"View", U"Toggle Semitone (log) / Linear Hz scale", 0, menu_cb_toggleScale);
}

autoVoxalysVrpRecorder VoxalysVrpRecorder_create (
	double f0Floor, double f0Ceiling,
	double splFloor, double splCeiling,
	double splCalibrationOffset_dB)
{
	try {
		Melder_require (f0Floor >= 20.0 && f0Ceiling > f0Floor,
			U"Voxalys VRP: F0 floor must be >= 20 Hz and strictly below F0 ceiling.");
		Melder_require (splCeiling > splFloor,
			U"Voxalys VRP: SPL ceiling must be greater than SPL floor.");

		if (! MelderAudio_hasBeenInitialized) {
			const PaError initErr = Pa_Initialize ();
			if (initErr != paNoError)
				Melder_throw (U"PortAudio initialization failed: ", Melder_peek8to32 (Pa_GetErrorText (initErr)));
			MelderAudio_hasBeenInitialized = true;
		}

		PaDeviceIndex defaultDev = Pa_GetDefaultInputDevice ();
		if (defaultDev == paNoDevice) {
			const PaDeviceIndex count = Pa_GetDeviceCount ();
			for (PaDeviceIndex i = 0; i < count; i ++) {
				const PaDeviceInfo *d = Pa_GetDeviceInfo (i);
				if (d && d -> maxInputChannels > 0) {
					defaultDev = i;
					break;
				}
			}
		}
		if (defaultDev == paNoDevice)
			Melder_throw (U"No audio input device available for Interactive VRP.");

		autoVoxalysVrpRecorder me = Thing_new (VoxalysVrpRecorder);
		my sampleRate = 44100.0;
		my f0Floor = std::min (50.0, f0Floor);
		my f0Ceiling = std::max (1100.0, f0Ceiling);
		my viewF0Min = f0Floor;
		my viewF0Max = f0Ceiling;
		my splFloor = splFloor;
		my splCeiling = splCeiling;
		my splCalibrationOffset_dB = splCalibrationOffset_dB;
		my useSemitoneScale = true;
		my recording = false;
		my paused = false;
		my hasLatestPoint = false;
		my portaudioStream = nullptr;
		my selectedDevice = defaultDev;
		my recordedSamples.store (0, std::memory_order_relaxed);
		/* Pre-allocate 3 minutes of mono 44.1 kHz audio (~31.7 MB) so audio callback never allocates. */
		my recordBuffer.assign (static_cast<size_t> (my sampleRate * 180.0), 0.0f);

		my rustStream = voxalys_stream_new (
			static_cast<float> (my sampleRate),
			65536,
			static_cast<float> (my f0Floor),
			static_cast<float> (my f0Ceiling),
			static_cast<float> (my splCalibrationOffset_dB)
		);
		if (! my rustStream)
			Melder_throw (U"Voxalys Rust stream creation failed: ", Melder_peek8to32 (voxalys_last_error ()));

		Editor_init (me.get(), 100, 100, 820, 620, U"Voxalys Interactive VRP (Live)", nullptr);
		my graphics = Graphics_create_xmdrawingarea (my canvas);
		Melder_assert (my graphics);

		struct structGuiDrawingArea_ResizeEvent event { my canvas, 0, 0 };
		event. width = GuiControl_getWidth (my canvas);
		event. height = GuiControl_getHeight (my canvas);
		gui_drawingarea_cb_resize (me.get(), & event);

		#if cocoa
			CFRunLoopTimerContext context = { 0, me.get(), nullptr, nullptr, nullptr };
			my d_cocoaTimer = CFRunLoopTimerCreate (nullptr, CFAbsoluteTimeGetCurrent () + 0.025,
				0.025, 0, 0, vrpTimerCallback, & context);
			CFRunLoopAddTimer (CFRunLoopGetCurrent (), my d_cocoaTimer, kCFRunLoopCommonModes);
		#elif gtk
			my gtkTimerId = g_timeout_add (25, vrpTimerCallback, me.get());
		#elif motif
			my workProcId = XtAddWorkProc (vrpTimerCallback, me.get());
		#endif

		updateStatusLabel (me.get());
		return me;
	} catch (MelderError) {
		Melder_throw (U"Voxalys Interactive VRP Recorder not created.");
	}
}

/* End of file Voxalys.cpp */
