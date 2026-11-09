#pragma once
#include <rack.hpp>

// Shared message layout for the invisible Intel <-> Command expander link.
//
// Both plugins carry an identical copy of this header: the enum defines the
// wire protocol between them, and keeping the struct and type IDs in lockstep
// is what keeps the memory layout compatible across the expander boundary.
//
// In VCV Rack v2, expanders can be chained horizontally in a contiguous row:
// each module writes into its own `leftExpander.producerMessage` /
// `rightExpander.producerMessage` and reads the neighbour's message via
// its `consumerMessage` slot. When two Grandsummoner modules sit side by side,
// they exchange these small typed structs each sample frame without any
// patch cables or user intervention.
//
// Layout contract:
// - `type` identifies what kind of data the message holds (see IntelLinkType).
// - Modules MUST check `type` before reading any other field.
// - Unknown message types are ignored silently.
// - Producer and consumer buffers are swapped by the engine on every frame
//   if `messageFlipRequested` was set.

enum IntelLinkType : uint32_t {
	INTEL_LINK_NONE        = 0,
	INTEL_LINK_MOD_MESSAGE = 0x494E544C, // 'INTL' - Intel's broadcast to neighbours
	INTEL_LINK_BEAMS_TICK  = 0x4245414D, // 'BEAM' - Beams master clock pulse
};

struct IntelModMessage {
	uint32_t type;            // must be INTEL_LINK_MOD_MESSAGE
	float    rate;            // Intel RATE pot + CV (normalized 0..1)
	float    depth;           // Intel DEPTH pot + CV (normalized 0..1)
	float    tone;            // Intel TONE pot + CV (normalized 0..1)
	float    speed;           // Intel SPEED pot + CV (normalized 0..1)
	float    trig[4];         // 4 trigger lane outputs (0 or 10V pulses)
	uint32_t activeLanesMask; // bitmask of lanes currently firing
	uint32_t version;         // protocol revision (currently 1)
};

struct BeamsTickMessage {
	uint32_t type;            // must be INTEL_LINK_BEAMS_TICK
	uint32_t tickCount;       // monotonically increasing tick sequence
	float    tempoBpm;        // current effective BPM of Beams' internal clock
	uint8_t  activeTrackMask; // bitmask of tracks that fired this tick (bits 0..3)
	uint8_t  _reserved[3];
};

// Maximum number of adjacent hops to walk when searching for Intel along the
// expander chain: Beams bumps beamsTick once per master clock tick
// and passes it outward.
static const int INTEL_LINK_MAX_HOPS = 32;

// Helper: checks whether a module pointer is non-null and belongs to the
// Grandsummoner family (by matching the plugin slug against our known family).
inline bool isIntelFamilyModule(rack::engine::Module* m) {
	if (!m || !m->model || !m->model->plugin) return false;
	const std::string& p = m->model->plugin->slug;
	return p == "Intel" || p == "Grandsummoner-Intel" ||
	       p == "SpacesCommand" || p == "Grandsummoner-SpacesCommand" ||
	       p == "Beams" || p == "Grandsummoner-Beams" ||
	       p == "Stellar" || p == "Grandsummoner-Stellar" ||
	       p == "Explorer" || p == "Grandsummoner-Explorer";
}

// Finds the nearest module with the given plugin/model slug by walking
// outward from `self` in one direction (right = true, else left) through
// the contiguous row of adjacent modules -- so the link works with other
// modules (Stellar, etc.) sitting in between, not just immediate
// neighbors. Stops at the first gap in the row.
inline rack::engine::Module* intelLinkFind(rack::engine::Module* self, bool right, const char* slug) {
	rack::engine::Module* m = right ? self->rightExpander.module : self->leftExpander.module;
	for (int hops = 0; m && hops < 32; hops++) {
		if (m->model && (m->model->slug == slug || (m->model->plugin && m->model->plugin->slug == slug)))
			return m;
		m = right ? m->rightExpander.module : m->leftExpander.module;
	}
	return nullptr;
}
