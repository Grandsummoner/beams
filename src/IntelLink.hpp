#pragma once
#include <rack.hpp>
// Shared message layout for the invisible Intel <-> Command expander link.
// This exact struct definition is duplicated verbatim in both the Intel and
// Spaces (Command) repos -- they're separate plugin binaries, so there's no
// shared header to include from; keeping the text identical in both copies
// is what keeps the memory layout compatible across the expander boundary.
// If you ever add/reorder fields here, make the identical edit in the other
// repo's copy or the two sides will silently misread each other.
struct IntelModMessage {
	// Continuous modulation offsets, already depth-scaled by Intel, roughly
	// in a -1..1 range (Command decides how to apply/scale/floor each one
	// against its own parameter's real range).
	float rateOffset = 0.f;
	float densOffset = 0.f;
	float swingOffset = 0.f;
	float entropyOffset = 0.f;
	// Static DEPTH-setting level per channel, 0 (no LFO) to 1 (max LFO) --
	// i.e. depthState/3, NOT the live oscillating value above. Command's
	// optional depth-gauge display reads these; actual modulation
	// application still uses the *Offset fields.
	float rateDepth = 0.f;
	float densDepth = 0.f;
	float swingDepth = 0.f;
	float entropyDepth = 0.f;
	// Global DECAY offset for Beams, 0..1, sent from Intel's TONE knob
	// (TONE stays Intel's own REVERB-page tone control; this is a second,
	// simultaneous use of that same knob's value, not a separate control).
	// Beams treats this as an additive offset over each track's own local
	// DECAY knob, same "generic offset, receiver scales against its own
	// real range" pattern as the *Offset fields above.
	float decayOffset = 0.f;
	// (v6) Beams -> Intel clock link, sent the OTHER way along the same
	// expander chain: Beams bumps beamsTick once per master clock tick
	// (its CLOCK input if patched, else its own TEMPO) and Intel derives
	// its four trigger outputs from those ticks (RATE = every 8th/4th/
	// 2nd/every tick), so Intel's triggers lock to Beams' grid instead of
	// free-running. beamsTickValid stays false until Beams has actually
	// sent one; Intel then falls back to its own free-running SPEED.
	unsigned int beamsTick = 0;
	bool beamsTickValid = false;
	//
	// This one struct type is reused for BOTH the Intel<->Command link
	// AND the separate Intel<->Beams link -- they're different message
	// instances (each module owns its own producer/consumer buffers for
	// its own expander slots, see the comment on Command's/Beams'
	// buffers), so there's no collision: Command's `present` means
	// "Intel is talking to COMMAND" on Command's own buffer instance,
	// Beams' `present` means "Intel is talking to BEAMS" on Beams' own,
	// separate buffer instance. Each side only reads the fields
	// relevant to it (Command: rate/dens/swing/entropy; Beams: decay).
	// True whenever Intel is actually present and sending on this
	// particular link.
	bool present = false;
};

// Finds the nearest module with the given plugin/model slug by walking
// outward from `self` in one direction (right = true, else left) through
// the contiguous row of adjacent modules -- so the link works with other
// modules (Stellar, etc.) sitting in between, not just immediate
// neighbors. Stops at the first gap in the row. Also identical in both
// repos' copies of this file.
inline rack::engine::Module* intelLinkFind(rack::engine::Module* self, bool right, const char* slug) {
	rack::engine::Module* m = right ? self->rightExpander.module : self->leftExpander.module;
	for (int hops = 0; m && hops < 32; hops++) {
		if (m->model && m->model->plugin && m->model->plugin->slug == slug && m->model->slug == slug)
			return m;
		m = right ? m->rightExpander.module : m->leftExpander.module;
	}
	return nullptr;
}
