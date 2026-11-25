#ifndef _Voxalys_h_
#define _Voxalys_h_
/* Voxalys.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2007-2026 Jonas Lindh
 *
 * Native C++ acoustic analysis, JoVoice Rust DSP bridge, and interactive
 * real-time PortAudio Voice Range Profile (VRP) Editor for Praat.
 */

#include <atomic>
#include <vector>

#include "Editor.h"
#include "Sound.h"
#include "Table.h"

#include "../external/portaudio/portaudio.h"
#include "../external/voxalys_rust/include/voxalys_rust.h"

/*
	Native acoustic & perturbation voice report combining Praat's cross-correlation
	pitch/pulse/harmonicity/LTAS analysis with the JoVoice Rust DSP engine.
	Outputs a formatted clinical report with Deliyski (1993) / MDVP pathology
	threshold flags into the Praat Info window.
*/
void Voxalys_analyzeSound (Sound me, double pitchFloor, double pitchCeiling, double splCalibrationOffset_dB);

/*
	Streams a Sound object through the lock-free Rust VRP streaming engine
	(the exact same pipeline used by the live PortAudio recorder) and returns
	a Table containing (time_s, f0_Hz, f0_st_re100Hz, spl_dB, voiced, quality, phonation).
*/
autoTable Voxalys_Sound_to_VRPTable (Sound me, double pitchFloor, double pitchCeiling, double splCalibrationOffset_dB);

Thing_define (VoxalysVrpRecorder, Editor) {
	double sampleRate;
	double f0Floor;
	double f0Ceiling;
	double splFloor;
	double splCeiling;
	double splCalibrationOffset_dB;

	bool recording;
	bool paused;

	/* Pre-allocated mono recording buffer accessed by PortAudio callback (no allocs/locks). */
	std::vector<float> recordBuffer;
	std::atomic<size_t> recordedSamples;

	/* Lock-free SPSC Rust stream handle & accumulated VRP trajectory points. */
	VoxalysStreamHandle *rustStream;
	std::vector<VoxalysVrpPoint> points;
	VoxalysVrpPoint latestPoint;
	bool hasLatestPoint;

	PaStream *portaudioStream;
	PaDeviceIndex selectedDevice;

	GuiDrawingArea canvas;
	GuiLabel statusLabel;
	GuiText recordingNameField;
	GuiButton startButton;
	GuiButton pauseButton;
	GuiButton clearButton;
	GuiButton saveButton;
	GuiButton closeButton;
	autoGraphics graphics;

	#if cocoa
		CFRunLoopTimerRef d_cocoaTimer;
	#elif gtk
		unsigned int gtkTimerId;
	#elif motif
		XtWorkProcId workProcId;
	#endif

	void v9_destroy () noexcept
		override;
	bool v_hasEditMenu ()
		override { return false; }
	bool v_scriptable ()
		override { return false; }
	void v_createChildren ()
		override;
	void v_createMenus ()
		override;
};

autoVoxalysVrpRecorder VoxalysVrpRecorder_create (
	double f0Floor, double f0Ceiling,
	double splFloor, double splCeiling,
	double splCalibrationOffset_dB
);

#endif /* _Voxalys_h_ */
