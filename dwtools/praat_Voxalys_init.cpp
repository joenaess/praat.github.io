/* praat_Voxalys_init.cpp
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2007-2026 Jonas Lindh
 *
 * Menu and action command registration for native Voxalys / JoVoice features.
 */

#include "praatM.h"
#include "Voxalys.h"

static VoxalysVrpRecorder theReferenceToTheOnlyVoxalysVrpRecorder = nullptr;

static void cb_VoxalysVrpRecorder_destruction (VoxalysVrpRecorder /* me */) {
	theReferenceToTheOnlyVoxalysVrpRecorder = nullptr;
}

static void cb_VoxalysVrpRecorder_publication (Editor /* me */, autoDaata publication) {
	try {
		praat_new (publication.move());
	} catch (MelderError) {
		Melder_flushError ();
	}
	praat_updateSelection ();
}

FORM (WINDOW_Voxalys_recordInteractiveVRP, U"Voxalys: Record Interactive VRP (live)", nullptr) {
	POSITIVE (f0Floor, U"F0 floor (Hz)", U"65.0")
	POSITIVE (f0Ceiling, U"F0 ceiling (Hz)", U"900.0")
	REAL (splFloor, U"SPL floor (dB)", U"40.0")
	REAL (splCeiling, U"SPL ceiling (dB)", U"115.0")
	REAL (splCalibrationOffset_dB, U"SPL calibration offset (dB)", U"94.0")
	OK
DO
	if (theReferenceToTheOnlyVoxalysVrpRecorder) {
		Editor_raise (theReferenceToTheOnlyVoxalysVrpRecorder);
	} else {
		autoVoxalysVrpRecorder recorder = VoxalysVrpRecorder_create (
			f0Floor, f0Ceiling, splFloor, splCeiling, splCalibrationOffset_dB
		);
		Editor_setDestructionCallback (recorder.get(), cb_VoxalysVrpRecorder_destruction);
		Editor_setPublicationCallback (recorder.get(), cb_VoxalysVrpRecorder_publication);
		theReferenceToTheOnlyVoxalysVrpRecorder = recorder.get();
		recorder.releaseToUser();
	}
	END_NO_NEW_DATA
}

FORM (INFO_ONE__Sound_voxalysAnalyzeVoice, U"Voxalys: Analyze voice & JoVoice metrics", nullptr) {
	POSITIVE (pitchFloor, U"Pitch floor (Hz)", U"75.0")
	POSITIVE (pitchCeiling, U"Pitch ceiling (Hz)", U"500.0")
	REAL (splCalibrationOffset_dB, U"SPL calibration offset (dB)", U"94.0")
	OK
DO
	INFO_ONE (Sound)
		Voxalys_analyzeSound (me, pitchFloor, pitchCeiling, splCalibrationOffset_dB);
	INFO_ONE_END
}

FORM (CONVERT_EACH_TO_ONE__Sound_voxalysToVRPTable, U"Voxalys: Compute VRP trajectory Table", nullptr) {
	POSITIVE (pitchFloor, U"Pitch floor (Hz)", U"65.0")
	POSITIVE (pitchCeiling, U"Pitch ceiling (Hz)", U"900.0")
	REAL (splCalibrationOffset_dB, U"SPL calibration offset (dB)", U"94.0")
	OK
DO
	CONVERT_EACH_TO_ONE (Sound)
		autoTable result = Voxalys_Sound_to_VRPTable (me, pitchFloor, pitchCeiling, splCalibrationOffset_dB);
	CONVERT_EACH_TO_ONE_END (my name.get(), U"_VRP")
}

void praat_Voxalys_init () {
	/* Objects -> New -> Voxalys */
	praat_addMenuCommand (U"Objects", U"New", U"Voxalys", nullptr, 0, nullptr);
	praat_addMenuCommand (U"Objects", U"New", U"Record Interactive VRP (live)...", U"Voxalys", 1 | GuiMenu_NO_API,
		WINDOW_Voxalys_recordInteractiveVRP);

	/* Sound dynamic action menu -> Voxalys - */
	praat_addAction1 (classSound, 0, U"Voxalys -", nullptr, 0, nullptr);
	praat_addAction1 (classSound, 1, U"Voxalys analyze voice & JoVoice metrics...", nullptr, 1,
		INFO_ONE__Sound_voxalysAnalyzeVoice);
	praat_addAction1 (classSound, 0, U"Voxalys compute VRP trajectory Table...", nullptr, 1,
		CONVERT_EACH_TO_ONE__Sound_voxalysToVRPTable);
}

/* End of file praat_Voxalys_init.cpp */
