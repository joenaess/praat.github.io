# voxalys.praat
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2007-2026 Jonas Lindh
#
# Regression tests for native Voxalys C++ / JoVoice Rust integration in Praat.

writeInfoLine: "Testing native Voxalys & JoVoice Rust engine..."

# 1. Synthesize a clean harmonic vowel at F0 = 150 Hz (1.0 s, 44.1 kHz).
clean = Create Sound from formula: "clean_vowel", 1, 0, 1.0, 44100,
	... "0.35 * (sin(2*pi*150*x) + 0.5*sin(2*pi*300*x) + 0.33*sin(2*pi*450*x) + 0.25*sin(2*pi*600*x) + 0.2*sin(2*pi*750*x))"

# 2. Test headless streaming VRP trajectory Table generation (same Rust SPSC engine as live UI).
selectObject: clean
vrp_table = Voxalys compute VRP trajectory Table: 65.0, 900.0, 94.0
n_rows = Get number of rows
assert n_rows >= 80 ; Expected >= 80 VRP frames for 1.0 s signal, got 'n_rows'

n_cols = Get number of columns
assert n_cols = 7 ; Expected 7 VRP table columns, got 'n_cols'

voiced_count = 0
f0_sum = 0
spl_sum = 0
qual_sum = 0
for row from 1 to n_rows
	v = Get value: row, "voiced"
	if v = 1
		voiced_count += 1
		f0 = Get value: row, "f0_Hz"
		spl = Get value: row, "spl_dB"
		qual = Get value: row, "quality"
		assert f0 >= 145 and f0 <= 155 ; Unexpected F0='f0' at row 'row'
		assert spl >= 65 and spl <= 105 ; Unexpected SPL='spl' at row 'row'
		assert qual > 0.5 and qual <= 1.0 ; Unexpected quality='qual' at row 'row'
		f0_sum += f0
		spl_sum += spl
		qual_sum += qual
	endif
endfor

assert voiced_count >= 70 ; Expected >= 70 voiced frames, got 'voiced_count'
mean_f0 = f0_sum / voiced_count
assert mean_f0 >= 148.5 and mean_f0 <= 151.5 ; Mean F0 'mean_f0' outside 148.5..151.5 Hz

# 3. Test native combined acoustic + JoVoice report on clean vowel.
selectObject: clean
Voxalys analyze voice & JoVoice metrics: 75.0, 500.0, 94.0
clean_report$ = info$()
assert index(clean_report$, "=== VOXALYS NATIVE VOICE & JOVOICE REPORT ===") > 0
assert index(clean_report$, "JoVoice Mean F0:") > 0
assert index(clean_report$, "VRP Area (st*dB):") > 0
assert index(clean_report$, "OK (<= 1.040 %)") > 0
assert index(clean_report$, "OK (<= 3.810 %)") > 0

# 4. Synthesize a strongly perturbed voice (FM jitter + AM shimmer) and verify pathology flagging.
perturbed = Create Sound from formula: "perturbed_voice", 1, 0, 1.0, 44100,
	... "(0.30 + 0.16 * sin(2*pi*19*x)) * (sin(2*pi*150*x + 2.5*sin(2*pi*23*x)) + 0.5*sin(2*pi*300*x + 5.0*sin(2*pi*23*x)))"

selectObject: perturbed
Voxalys analyze voice & JoVoice metrics: 75.0, 500.0, 94.0
perturbed_report$ = info$()
assert index(perturbed_report$, "ABOVE THRESHOLD") > 0

# 5. Clean up all temporary Praat objects (zero memory leaks).
removeObject: clean, vrp_table, perturbed

writeInfoLine: "OK: All native Voxalys & JoVoice tests passed (mean F0 = ", fixed$(mean_f0, 2), " Hz, voiced frames = ", voiced_count, ")."
