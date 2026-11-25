# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2007-2026 Jonas Lindh
#
# Makefile wrapper for building the Voxalys / JoVoice Rust static library.

CARGO ?= cargo

.PHONY: all clean test

all: target/release/libvoxalys_rust.a

target/release/libvoxalys_rust.a: Cargo.toml src/lib.rs src/ffi.rs src/jovoice_bridge.rs src/ring_buffer.rs src/vrp_analyzer.rs
	$(CARGO) build --release --manifest-path Cargo.toml

test:
	$(CARGO) test --manifest-path Cargo.toml

clean:
	$(CARGO) clean --manifest-path Cargo.toml
