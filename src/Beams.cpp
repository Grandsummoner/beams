#include "plugin.hpp"
#include "IntelLink.hpp"
#include "BeamsLayout.hpp"
#include <osdialog.h>
#include <system.hpp>
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>
#include <fstream>

// ---------------------------------------------------------------------
// Euclidean rhythm generator
// ---------------------------------------------------------------------
// Evenly spreads `k` hits across `n` steps with a hit ALWAYS on step 0
// (the standard Euclidean placement: E(4,8) = x.x.x.x., E(3,8) =
// x..x..x., E(2,8) = x...x...). The test is ((i*k) mod n) < k -- a
// closed form of the same even-distribution idea as Bjorklund's
// algorithm, without the recursion. An earlier Bresenham-accumulator
// version spread hits just as evenly but started a step late, so a
// 4-of-8 kick landed on the off-beats at ROT 0.
static std::vector<bool> euclideanPattern(int n, int k) {
	std::vector<bool> pattern(std::max(n, 1), false);
	if (n <= 0) return pattern;
	k = clamp(k, 0, n);
	if (k <= 0) return pattern;
	if (k >= n) { pattern.assign(n, true); return pattern; }
	for (int i = 0; i < n; i++)
		pattern[i] = ((i * k) % n) < k;
	return pattern;
}

// ---------------------------------------------------------------------
// Per-track sample bank + voice state
// ---------------------------------------------------------------------
struct SampleSlot {
	std::vector<float> data;  // mono, already resampled to the engine's rate
	bool loaded = false;
};

// Decodes up to 4 .wav files (alphabetical) from `dir` into `kit`,
// resampling each to `engineRate` with simple linear interpolation
// (adequate for a one-time load; TUNE already does linear-interpolated
// playback, so this matches that quality tier). Runs entirely on local
// data -- nothing the audio thread touches -- so the module only needs
// to take its lock for the final swap (see Beams::loadTrackFolder).
struct LoadedKit {
	SampleSlot slots[4];
	int count = 0;
};

static bool decodeKit(const std::string& dir, float engineRate, LoadedKit& kit) {
	std::vector<std::string> entries = system::getEntries(dir);
	std::vector<std::string> wavs;
	for (auto& e : entries) {
		std::string lower = e;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
		if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".wav")
			wavs.push_back(e);
	}
	std::sort(wavs.begin(), wavs.end());
	if (wavs.empty()) return false;
	if (wavs.size() > 4) wavs.resize(4);

	int count = 0;
	for (auto& path : wavs) {
		unsigned int channels = 0, sampleRate = 0;
		drwav_uint64 totalFrames = 0;
		float* raw = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &sampleRate, &totalFrames, NULL);
		if (!raw) continue;
		if (channels == 0) { drwav_free(raw, NULL); continue; }

		// Fold to mono.
		std::vector<float> mono(totalFrames);
		for (drwav_uint64 f = 0; f < totalFrames; f++) {
			float sum = 0.f;
			for (unsigned int c = 0; c < channels; c++) sum += raw[f * channels + c];
			mono[f] = sum / (float) channels;
		}
		drwav_free(raw, NULL);

		SampleSlot& slot = kit.slots[count];
		if (sampleRate != (unsigned int) engineRate && sampleRate > 0 && !mono.empty()) {
			double ratio = (double) sampleRate / (double) engineRate;
			size_t outLen = (size_t) (mono.size() / ratio);
			slot.data.resize(outLen);
			for (size_t o = 0; o < outLen; o++) {
				double srcPos = o * ratio;
				size_t i0 = (size_t) srcPos;
				size_t i1 = std::min(i0 + 1, mono.size() - 1);
				float frac = (float) (srcPos - i0);
				slot.data[o] = mono[i0] * (1.f - frac) + mono[i1] * frac;
			}
		}
		else {
			slot.data = mono;
		}
		slot.loaded = true;
		count++;
	}
	kit.count = count;
	return count > 0;
}

struct TrackVoice {
	SampleSlot slots[4];
	int loadedCount = 0;
	int rrIndex = 0;      // next slot to play in SEQ mode
	int lastPlayed = -1;  // for RND's avoid-immediate-repeat

	// Playback state
	int activeSlot = -1;
	double playPos = 0.0;
	float envAttack = 0.f;   // 0..1, ramps up over ~3ms at trigger start
	float envValue = 1.f;    // decay multiplier, 1 at trigger, falls toward 0
	float velocity = 1.f;

	// Euclidean step state
	int stepCounter = -1;    // pre-incremented on first pulse -> starts at 0
	int cachedLen = -1, cachedSteps = -1;
	std::vector<bool> cachedPattern;

	dsp::SchmittTrigger trigInTrig;
	// (v3) an external trigger that arrived mid-tick waits here for the next
	// master clock tick (see the quantize rule in process())
	bool pendingTrig = false;
	float pendingVel = 1.f;
	float muteGain = 1.f;   // smoothed MUTE (~3ms) so muting never clicks
	std::string folder;   // last folder loaded -- saved in the patch
	bool rrMode = false;  // false = SEQ, true = RND
	bool fillOn = false;
	bool invOn = false;

	bool stepHit(int len, int steps, int rot) {
		if (len != cachedLen || steps != cachedSteps) {
			cachedPattern = euclideanPattern(len, steps);
			cachedLen = len;
			cachedSteps = steps;
		}
		int idx = ((stepCounter + rot) % len + len) % len;
		bool hit = cachedPattern[idx];
		if (fillOn) return true;
		if (invOn) return !hit;
		return hit;
	}

	void trigger(float vel) {
		if (loadedCount <= 0) return;
		int next;
		if (rrMode) {
			// Random, avoiding an immediate repeat when more than 1 is loaded.
			if (loadedCount == 1) {
				next = 0;
			}
			else {
				do {
					next = (int) std::floor(random::uniform() * loadedCount);
				} while (next == lastPlayed);
			}
		}
		else {
			next = rrIndex;
			rrIndex = (rrIndex + 1) % loadedCount;
		}
		lastPlayed = next;
		activeSlot = next;
		playPos = 0.0;
		envAttack = 0.f;
		envValue = 1.f;
		velocity = vel;
	}
};

// ---------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------
struct Beams : Module {
	enum ParamId {
		TEMPO_PARAM,
		ENUMS(LEVEL_PARAM, 4), ENUMS(TUNE_PARAM, 4), ENUMS(DECAY_PARAM, 4),
		ENUMS(LEN_PARAM, 4), ENUMS(STEPS_PARAM, 4), ENUMS(ROT_PARAM, 4),
		ENUMS(RRMODE_PARAM, 4), ENUMS(FILL_PARAM, 4), ENUMS(INV_PARAM, 4),
		ENUMS(MUTE_PARAM, 4),
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		ENUMS(TRIG_INPUT, 4),
		INPUTS_LEN
	};
	enum OutputId {
		MASTER_L_OUTPUT, MASTER_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LINK_LEFT_LIGHT, LINK_RIGHT_LIGHT,
		LIGHTS_LEN
	};

	TrackVoice tracks[4];
	dsp::SchmittTrigger clockInTrig;
	float internalPhase = 0.f;

	// Master clock bookkeeping. One "tick" = one 16th-note pulse from the
	// CLOCK input (if patched) or from TEMPO. tickCounter is also sent to
	// Intel over the link so Intel's triggers can be exact divisions of it.
	unsigned int tickCounter = 0;
	float timeSinceTick = 1e9f;   // seconds since the last tick
	float tickPeriod = 0.f;       // seconds between ticks (0 = unknown yet)

	// Intel link -- same buffer-ownership convention as Command's own
	// copy: allocated here, not by Intel, since the sending convention
	// is "write into the NEIGHBOR's own producer buffer".
	IntelModMessage leftIntelProducer, leftIntelConsumer, rightIntelProducer, rightIntelConsumer;
	bool intelPresent = false;
	float intelDecayOffset = 0.f;

	Beams() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TEMPO_PARAM, 5.f, 400.f, 120.f, "Tempo (master clock when CLOCK is unpatched)", " BPM");
		paramQuantities[TEMPO_PARAM]->randomizeEnabled = false;   // a random 5-400 BPM is never what you want
		configInput(CLOCK_INPUT, "Clock");
		for (int i = 0; i < 4; i++) {
			configParam(LEVEL_PARAM + i, 0.f, 1.f, 0.8f, string::f("Track %d level", i + 1));
			configParam(TUNE_PARAM + i, -1.f, 1.f, 0.f, string::f("Track %d tune", i + 1), " st", 0.f, 12.f);
			configParam(DECAY_PARAM + i, 0.f, 1.f, 0.6f, string::f("Track %d decay", i + 1));
			auto* lenQ = configParam(LEN_PARAM + i, 1.f, 32.f, 16.f, string::f("Track %d step length", i + 1));
			lenQ->snapEnabled = true;
			auto* stepsQ = configParam(STEPS_PARAM + i, 0.f, 32.f, 4.f, string::f("Track %d steps (hits)", i + 1));
			stepsQ->snapEnabled = true;
			auto* rotQ = configParam(ROT_PARAM + i, 0.f, 31.f, 0.f, string::f("Track %d rotation", i + 1));
			rotQ->snapEnabled = true;
			configSwitch(RRMODE_PARAM + i, 0.f, 1.f, 0.f, string::f("Track %d round-robin mode", i + 1), {"Sequential", "Random"});
			configSwitch(FILL_PARAM + i, 0.f, 1.f, 0.f, string::f("Track %d fill", i + 1), {"Off", "On"});
			configSwitch(INV_PARAM + i, 0.f, 1.f, 0.f, string::f("Track %d inverse fill", i + 1), {"Off", "On"});
			configSwitch(MUTE_PARAM + i, 0.f, 1.f, 0.f, string::f("Track %d mute (silences the track but the pattern keeps running)", i + 1), {"Off", "On"});
			paramQuantities[MUTE_PARAM + i]->randomizeEnabled = false;   // Randomize must never silence a track
			paramQuantities[LEVEL_PARAM + i]->randomizeEnabled = false;  // ...or jump the levels
			configInput(TRIG_INPUT + i, string::f("Track %d trigger", i + 1));
		}
		configOutput(MASTER_L_OUTPUT, "Master L");
		configOutput(MASTER_R_OUTPUT, "Master R");

		leftExpander.producerMessage = &leftIntelProducer;
		leftExpander.consumerMessage = &leftIntelConsumer;
		rightExpander.producerMessage = &rightIntelProducer;
		rightExpander.consumerMessage = &rightIntelConsumer;
	}

	static bool isIntel(Module* m) {
		return m && m->model && m->model->plugin && m->model->plugin->slug == "Intel" && m->model->slug == "Intel";
	}
	static bool isCommand(Module* m) {
		return m && m->model && m->model->plugin && m->model->plugin->slug == "SpacesCommand" && m->model->slug == "SpacesCommand";
	}
	static bool isStellarModule(Module* m) {
		return m && m->model && m->model->plugin && m->model->plugin->slug == "Stellar" && m->model->slug == "Stellar";
	}

	// Guards sample buffers against the UI thread swapping a freshly
	// decoded kit in while the audio thread is reading the old one.
	// Decoding happens outside the lock; only the swap is locked, and the
	// audio thread uses try_lock (skips playback for that sample rather
	// than ever blocking).
	std::mutex sampleMutex;

	bool loadTrackFolder(int track, const std::string& dir, float engineRate) {
		LoadedKit kit;
		if (!decodeKit(dir, engineRate, kit)) return false;
		std::lock_guard<std::mutex> g(sampleMutex);
		TrackVoice& t = tracks[track];
		for (int i = 0; i < 4; i++) t.slots[i] = std::move(kit.slots[i]);
		t.loadedCount = kit.count;
		t.rrIndex = 0;
		t.lastPlayed = -1;
		t.activeSlot = -1;
		t.folder = dir;
		return true;
	}

	void onReset() override {
		std::lock_guard<std::mutex> g(sampleMutex);
		for (int i = 0; i < 4; i++) tracks[i] = TrackVoice();
	}

	// Samples live on disk, not in the patch: save each track's folder
	// path and re-decode on load. A folder that has moved just leaves
	// that track empty (press LOAD again).
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* arr = json_array();
		for (int i = 0; i < 4; i++)
			json_array_append_new(arr, json_string(tracks[i].folder.c_str()));
		json_object_set_new(root, "folders", arr);
		return root;
	}
	void dataFromJson(json_t* root) override {
		json_t* arr = json_object_get(root, "folders");
		if (!arr) return;
		for (int i = 0; i < 4; i++) {
			json_t* js = json_array_get(arr, i);
			if (js && json_is_string(js)) {
				std::string p = json_string_value(js);
				if (!p.empty()) loadTrackFolder(i, p, APP->engine->getSampleRate());
			}
		}
	}

	void process(const ProcessArgs& args) override {
		// --- Intel link: read whatever's there this frame (decay offset
		// only -- Beams has no modulation targets of its own beyond that
		// one knob-sharing trick) ---
		// Intel may be anywhere along the contiguous row (Stellar etc. can
		// sit in between) -- Intel finds Beams the same way and writes
		// directly into whichever of OUR buffers faces back toward it,
		// bypassing whatever's actually adjacent on that side. So reading
		// must also use intelLinkFind here, not a raw adjacency check:
		// the true immediate neighbor on that side might not be Intel
		// itself, but Intel's data still landed in that buffer. Same
		// fix, same reason, as Command's own copy of this logic.
		bool chainLeft = (bool) intelLinkFind(this, false, "Intel");
		bool chainRight = (bool) intelLinkFind(this, true, "Intel");
		IntelModMessage intelMsg;
		if (chainLeft) intelMsg = *(IntelModMessage*)leftExpander.consumerMessage;
		else if (chainRight) intelMsg = *(IntelModMessage*)rightExpander.consumerMessage;
		intelPresent = intelMsg.present;
		intelDecayOffset = intelMsg.decayOffset;

		// LINK lights stay adjacency-only, matching the rest of the
		// family's convention (the light means "something's physically
		// touching me here", not "the data link is active").
		bool adjLeft = isIntel(leftExpander.module);
		bool adjRight = isIntel(rightExpander.module);
		lights[LINK_LEFT_LIGHT].setBrightness(adjLeft || isCommand(leftExpander.module) || isStellarModule(leftExpander.module) ? 1.f : 0.f);
		lights[LINK_RIGHT_LIGHT].setBrightness(adjRight || isCommand(rightExpander.module) || isStellarModule(rightExpander.module) ? 1.f : 0.f);

		// --- Shared clock: external CLOCK input overrides internal TEMPO ---
		bool clockPatched = inputs[CLOCK_INPUT].isConnected();
		bool pulse = false;
		if (clockPatched) {
			pulse = clockInTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f);
		}
		else {
			float bpm = params[TEMPO_PARAM].getValue();
			float pulseHz = (bpm / 60.f) * 4.f;  // 16th notes
			internalPhase += pulseHz * args.sampleTime;
			if (internalPhase >= 1.f) {
				internalPhase -= 1.f;
				pulse = true;
			}
		}

		// Tick bookkeeping + clock link to Intel. Master priority: CLOCK input
		// if patched, else TEMPO; Intel's trigger RATE is a division of this.
		timeSinceTick = std::min(timeSinceTick + args.sampleTime, 1e6f);
		if (pulse) {
			if (clockPatched) tickPeriod = (timeSinceTick < 5.f) ? timeSinceTick : 0.f;   // measured; a long gap = clock just (re)started, period unknown
			else tickPeriod = 15.f / std::max(params[TEMPO_PARAM].getValue(), 1.f);
			timeSinceTick = 0.f;
			tickCounter++;
			Module* intelL = intelLinkFind(this, false, "Intel");   // Intel to our left: write into ITS right-side buffer
			Module* intelR = intelLinkFind(this, true, "Intel");
			IntelModMessage tmsg;
			tmsg.beamsTick = tickCounter;
			tmsg.beamsTickValid = true;
			if (intelL) {
				*(IntelModMessage*)intelL->rightExpander.producerMessage = tmsg;
				intelL->rightExpander.messageFlipRequested = true;
			}
			if (intelR) {
				*(IntelModMessage*)intelR->leftExpander.producerMessage = tmsg;
				intelR->leftExpander.messageFlipRequested = true;
			}
		}

		std::unique_lock<std::mutex> sampleLock(sampleMutex, std::try_to_lock);
		bool canPlay = sampleLock.owns_lock();  // false only while a freshly loaded kit is being swapped in

		float mixL = 0.f, mixR = 0.f;
		for (int i = 0; i < 4; i++) {
			TrackVoice& t = tracks[i];

			// --- Toggles: these are latched switch params, so the param
			// value IS the state. (An earlier version edge-detected the
			// param and flipped a separate internal flag, which fell out
			// of sync with the visible button on every second click.) ---
			t.rrMode = params[RRMODE_PARAM + i].getValue() > 0.5f;
			t.fillOn = params[FILL_PARAM + i].getValue() > 0.5f;
			t.invOn = params[INV_PARAM + i].getValue() > 0.5f;

			// --- Triggering: external TRIG input overrides internal Euclidean ---
			bool patched = inputs[TRIG_INPUT + i].isConnected();
			if (patched) {
				// A patched TRIG replaces this track's own pattern, and is
				// a division of the master clock: it lands on the tick.
				//  - Arrives within ~4ms after a tick (Intel's triggers do,
				//    since Intel fires ON Beams' ticks): play now.
				//  - Arrives mid-tick (other modules): wait for the next
				//    tick -- unless that wait would be long (slow clocks,
				//    ambient tempos), the clock period is unknown, or the
				//    clock has stalled, in which case play immediately.
				float v = inputs[TRIG_INPUT + i].getVoltage();
				if (t.trigInTrig.process(v, 0.1f, 2.f)) {
					float vel = clamp(v / 10.f, 0.1f, 1.f);
					bool playNow = timeSinceTick <= 0.004f
						|| tickPeriod <= 0.f
						|| timeSinceTick > 2.f * tickPeriod
						|| (tickPeriod - timeSinceTick) > 0.25f;
					if (playNow) { if (canPlay) t.trigger(vel); }
					else { t.pendingTrig = true; t.pendingVel = vel; }
				}
				if (pulse && t.pendingTrig) {
					t.pendingTrig = false;
					if (canPlay) t.trigger(t.pendingVel);
				}
			}
			else if (pulse) {
				t.pendingTrig = false;
				int len = (int) std::round(params[LEN_PARAM + i].getValue());
				int steps = (int) std::round(params[STEPS_PARAM + i].getValue());
				int rot = (int) std::round(params[ROT_PARAM + i].getValue());
				t.stepCounter = (t.stepCounter + 1) % len;
				if (t.stepHit(len, steps, rot) && canPlay) t.trigger(1.f);
			}

			// MUTE: ~3ms fade so toggling never clicks and a muted track never
			// blips on its next hit (updated every sample, not only while a
			// voice is sounding). The voice keeps advancing underneath, so
			// unmuting rejoins the pattern in time.
			float muteTarget = params[MUTE_PARAM + i].getValue() > 0.5f ? 0.f : 1.f;
			t.muteGain += (muteTarget - t.muteGain) * std::min(1.f, args.sampleTime / 0.003f);

			// --- Playback: resampled (TUNE) mono read, attack+decay envelope ---
			float voice = 0.f;
			if (canPlay && t.activeSlot >= 0 && t.loadedCount > 0) {
				SampleSlot& slot = t.slots[t.activeSlot];
				if (!slot.data.empty() && (size_t) t.playPos < slot.data.size()) {
					size_t i0 = (size_t) t.playPos;
					size_t i1 = std::min(i0 + 1, slot.data.size() - 1);
					float frac = (float) (t.playPos - i0);
					float s = slot.data[i0] * (1.f - frac) + slot.data[i1] * frac;

					// Attack ramp (~3ms) -- declicks both the sample's own
					// start and any retrigger that interrupts a still-
					// playing previous hit.
					t.envAttack = std::min(1.f, t.envAttack + args.sampleTime / 0.003f);

					// Decay: knob + Intel's offset (additive, clamped),
					// mapped to a time constant from ~5ms to ~2s.
					float decayParam = clamp(params[DECAY_PARAM + i].getValue() + (intelPresent ? intelDecayOffset - 0.5f : 0.f), 0.f, 1.f);
					float decayTime = 0.005f + decayParam * decayParam * 2.f;
					float decayCoeff = std::exp(-args.sampleTime / decayTime);
					t.envValue *= decayCoeff;

					voice = s * t.envAttack * t.envValue * t.velocity * params[LEVEL_PARAM + i].getValue();
					voice *= t.muteGain;

					float tuneSemis = params[TUNE_PARAM + i].getValue() * 12.f;
					float rateMul = std::pow(2.f, tuneSemis / 12.f);
					t.playPos += rateMul;
				}
				else {
					t.activeSlot = -1;
				}
			}
			mixL += voice;
			mixR += voice;
		}

		outputs[MASTER_L_OUTPUT].setVoltage(clamp(mixL, -1.f, 1.f) * 5.f);
		outputs[MASTER_R_OUTPUT].setVoltage(clamp(mixR, -1.f, 1.f) * 5.f);
	}
};

// ---------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------
// NOTE on units: NanoVG draws in pixels, but the panel is laid out in
// mm. Every fixed metric below goes through mm() so strokes/radii/dot
// pitches match the printed guides (an earlier version used raw mm
// numbers as pixels, which made dots/strokes ~3x too small).
static float mm(float v) {
	return mm2px(Vec(v, 0.f)).x;
}

// Places a widget by its CENTER with an explicit size. Sizes are set
// BEFORE positioning: createParamCentered() centers using the box size
// at creation time, which is zero for these custom widgets, so
// anything resized afterwards ended up offset by half its size.
template <class T>
static T* placeParam(Module* module, int paramId, float cxMm, float cyMm, float wMm, float hMm) {
	T* w = createParam<T>(Vec(), module, paramId);
	w->box.size = mm2px(Vec(wMm, hMm));
	w->box.pos = mm2px(Vec(cxMm, cyMm)).minus(w->box.size.div(2.f));
	return w;
}

// Bold text: Rack ships DejaVuSans regular only, so bold is faked by
// overdrawing the glyphs at tiny offsets. Centered on (x, y).
static void drawBoldText(NVGcontext* vg, float x, float y, const char* text, float sizeMm, NVGcolor color) {
	std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
	if (!font || font->handle < 0) return;
	nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, mm(sizeMm));
	nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, color);
	float o = mm(0.09f);
	nvgText(vg, x - o, y, text, NULL);
	nvgText(vg, x + o, y, text, NULL);
	nvgText(vg, x, y - o * 0.6f, text, NULL);
	nvgText(vg, x, y + o * 0.6f, text, NULL);
	nvgText(vg, x, y, text, NULL);
}

// Where the LOAD folder dialog opens. Remembered across all four tracks,
// all Beams instances, and across Rack sessions (stored in Rack's user
// folder). It is the PARENT of the last folder you picked, so sibling
// kit folders (kicks / snares / hats ...) are one click away.
static std::string& browseDir() {
	static std::string dir;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		std::ifstream f(asset::user("Beams-lastdir.txt"));
		std::string line;
		if (f && std::getline(f, line)) dir = line;
	}
	return dir;
}
static void rememberBrowseDir(const std::string& pickedFolder) {
	std::string parent = system::getDirectory(pickedFolder);
	if (parent.empty()) return;
	browseDir() = parent;
	std::ofstream f(asset::user("Beams-lastdir.txt"));
	if (f) f << parent << "\n";
}

// Generic rotary knob, fully custom-drawn (no SVG knob asset) -- same
// approach as StellarKnob/MorphKnob/IntelKnob elsewhere in the family.
// Sized per-instance via box.size; the ring is drawn so its OUTER edge
// lands exactly on the printed guide ring.
struct BeamsKnob : ParamWidget {
	NVGcolor ringColor = nvgRGB(0x4A, 0x44, 0x38);

	BeamsKnob() {
		// 6.0mm, same as Intel's FX knobs (BeamsLayout::KNOB_SIZE).
		box.size = mm2px(Vec(BeamsLayout::KNOB_SIZE, BeamsLayout::KNOB_SIZE));
	}

	void draw(const DrawArgs& args) override {
		float r = box.size.x / 2.f;
		float cx = r, cy = r;
		ParamQuantity* pq = getParamQuantity();
		float value = pq ? pq->getValue() : 0.f;
		float minV = pq ? pq->getMinValue() : 0.f;
		float maxV = pq ? pq->getMaxValue() : 1.f;
		float norm = (maxV > minV) ? clamp((value - minV) / (maxV - minV), 0.f, 1.f) : 0.f;
		float sw = mm(0.45f);

		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, r - sw / 2.f);
		nvgFillColor(args.vg, nvgRGB(0xED, 0xE7, 0xDC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, ringColor);
		nvgStrokeWidth(args.vg, sw);
		nvgStroke(args.vg);

		// Pointer -- same -0.75pi..+0.75pi convention as the rest of the
		// family's knobs (MorphKnob/Trimpot/etc).
		float angle = -0.75f * (float)M_PI + norm * 1.5f * (float)M_PI;
		float pr = r - sw - mm(0.3f);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, cx + std::sin(angle) * pr * 0.25f, cy - std::cos(angle) * pr * 0.25f);
		nvgLineTo(args.vg, cx + std::sin(angle) * pr, cy - std::cos(angle) * pr);
		nvgStrokeColor(args.vg, nvgRGB(0x2A, 0x26, 0x20));
		nvgStrokeWidth(args.vg, mm(0.5f));
		nvgLineCap(args.vg, NVG_ROUND);
		nvgStroke(args.vg);
	}

	void onDragMove(const DragMoveEvent& e) override {
		ParamQuantity* pq = getParamQuantity();
		if (!pq) return;
		float range = pq->getMaxValue() - pq->getMinValue();
		float delta = -e.mouseDelta.y * range / 200.f;
		if ((APP->window->getMods() & RACK_MOD_MASK) == GLFW_MOD_SHIFT) delta *= 0.1f;
		pq->setValue(clamp(pq->getValue() + delta, pq->getMinValue(), pq->getMaxValue()));
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			createContextMenu();
			e.consume(this);
			return;
		}
		ParamWidget::onButton(e);
	}
};

// SMPL toggle: how the track steps through its (up to 4) loaded samples.
// Unlit = in order (sequential round-robin); lit blue = random order. The
// label is always "SMPL"; the hover tooltip spells out the mode.
struct RRModeButton : ParamWidget {
	RRModeButton() {
		box.size = mm2px(Vec(BeamsLayout::RR_W, BeamsLayout::RR_H));
	}
	void draw(const DrawArgs& args) override {
		ParamQuantity* pq = getParamQuantity();
		bool rnd = pq && pq->getValue() > 0.5f;
		float sw = mm(0.4f);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, sw / 2.f, sw / 2.f, box.size.x - sw, box.size.y - sw, mm(0.5f));
		nvgFillColor(args.vg, rnd ? nvgRGB(0x3E, 0x7C, 0xB8) : nvgRGB(0xED, 0xE7, 0xDC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, rnd ? nvgRGB(0x3E, 0x7C, 0xB8) : nvgRGB(0x4A, 0x44, 0x38));
		nvgStrokeWidth(args.vg, sw);
		nvgStroke(args.vg);
		drawBoldText(args.vg, box.size.x / 2.f, box.size.y / 2.f, "SMPL", 1.8f,
			rnd ? nvgRGB(0xF5, 0xF1, 0xE9) : nvgRGB(0x4A, 0x44, 0x38));
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			ParamQuantity* pq = getParamQuantity();
			if (pq) pq->setValue(pq->getValue() > 0.5f ? 0.f : 1.f);
			e.consume(this);
		}
	}
};

// FILL / INV toggles. Each lights in its OWN colour when active (FILL
// green, INV orange -- no purple), with a soft glow, and carries its own
// bold label.
struct FillToggleButton : ParamWidget {
	NVGcolor onColor = nvgRGB(0x24, 0x9A, 0x62);
	const char* label = "FILL";

	FillToggleButton() {
		box.size = mm2px(Vec(BeamsLayout::FILL_W, BeamsLayout::FILL_H));
	}
	void draw(const DrawArgs& args) override {
		ParamQuantity* pq = getParamQuantity();
		bool on = pq && pq->getValue() > 0.5f;
		float sw = mm(0.4f);
		if (on) {
			// glow: two soft halos just outside the button
			for (int k = 2; k >= 1; k--) {
				NVGcolor g = onColor;
				g.a = 0.16f * (3 - k);
				float grow = mm(0.35f) * k;
				nvgBeginPath(args.vg);
				nvgRoundedRect(args.vg, -grow, -grow, box.size.x + 2.f * grow, box.size.y + 2.f * grow, mm(0.4f) + grow);
				nvgFillColor(args.vg, g);
				nvgFill(args.vg);
			}
		}
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, sw / 2.f, sw / 2.f, box.size.x - sw, box.size.y - sw, mm(0.4f));
		nvgFillColor(args.vg, on ? onColor : nvgRGB(0xED, 0xE7, 0xDC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, on ? onColor : nvgRGB(0x4A, 0x44, 0x38));
		nvgStrokeWidth(args.vg, sw);
		nvgStroke(args.vg);
		drawBoldText(args.vg, box.size.x / 2.f, box.size.y / 2.f, label, 1.8f,
			on ? nvgRGB(0xFF, 0xFF, 0xFF) : nvgRGB(0x2E, 0x28, 0x1F));
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			ParamQuantity* pq = getParamQuantity();
			if (pq) pq->setValue(pq->getValue() > 0.5f ? 0.f : 1.f);
			e.consume(this);
		}
	}
};

// LOAD button -- not a param at all, a direct action: opens a folder
// picker (Rack's built-in osdialog, no extra dependency) and decodes up
// to 4 .wav files into the owning track via dr_wav.
struct LoadButton : OpaqueWidget {
	Beams* module = nullptr;
	int track = 0;

	LoadButton() {
		box.size = mm2px(Vec(BeamsLayout::LOAD_W, BeamsLayout::LOAD_H));
	}
	void draw(const DrawArgs& args) override {
		float sw = mm(0.4f);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, sw / 2.f, sw / 2.f, box.size.x - sw, box.size.y - sw, mm(0.6f));
		nvgFillColor(args.vg, nvgRGB(0xED, 0xE7, 0xDC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x4A, 0x44, 0x38));
		nvgStrokeWidth(args.vg, sw);
		nvgStroke(args.vg);
		drawBoldText(args.vg, box.size.x / 2.f, box.size.y / 2.f, "LOAD", 2.6f, nvgRGB(0x4A, 0x44, 0x38));
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			e.consume(this);
			if (!module) return;
			// Open where the last load happened (parent of the last folder
			// picked); on a fresh install, fall back to this track's own
			// saved folder, else the OS default.
			std::string start = browseDir();
			if (start.empty() && !module->tracks[track].folder.empty())
				start = system::getDirectory(module->tracks[track].folder);
			char* path = osdialog_file(OSDIALOG_OPEN_DIR, start.empty() ? NULL : start.c_str(), NULL, NULL);
			if (path) {
				std::string picked = path;
				free(path);
				rememberBrowseDir(picked);
				module->loadTrackFolder(track, picked, APP->engine->getSampleRate());
			}
		}
	}
};

// Round-robin dot readout -- 4 dots, fixed identity colors (reusing
// Intel's FX quartet), filled when that slot has a sample loaded. The
// widget box is the printed readout rectangle itself, so the dots land
// exactly on the guide dots (first dot 1.4mm in, 2.33mm pitch).
struct RRDotsWidget : Widget {
	Beams* module = nullptr;
	int track = 0;
	static constexpr int N = 4;
	NVGcolor colors[N] = {
		nvgRGB(0x8A, 0x64, 0x23), nvgRGB(0x8A, 0x2A, 0x2A),
		nvgRGB(0x2E, 0x4A, 0x6E), nvgRGB(0x6B, 0x4C, 0x8A),
	};

	RRDotsWidget() {
		box.size = mm2px(Vec(BeamsLayout::DOTS_BOX_W, BeamsLayout::DOTS_BOX_H));
	}
	void draw(const DrawArgs& args) override {
		int loaded = module ? module->tracks[track].loadedCount : 0;
		// Slot that played most recently -- this is the round-robin
		// position indicator: it steps along the dots on every hit (in
		// SEQ order, or jumping around in RND), so you can see the
		// rotation working. Loaded-but-idle dots are dimmer.
		int current = module ? module->tracks[track].lastPlayed : -1;
		float cy = box.size.y / 2.f;
		float pitch = mm(BeamsLayout::DOTS_PITCH);
		for (int d = 0; d < N; d++) {
			float dx = mm(BeamsLayout::DOTS_PAD) + d * pitch;
			bool isLoaded = d < loaded;
			bool isCurrent = isLoaded && d == current;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, dx, cy, mm(BeamsLayout::DOTS_R));
			if (isLoaded) {
				NVGcolor c = colors[d];
				if (!isCurrent) c.a = 0.35f;
				nvgFillColor(args.vg, c);
				nvgFill(args.vg);
			}
			nvgStrokeColor(args.vg, colors[d]);
			nvgStrokeWidth(args.vg, mm(0.35f));
			nvgStroke(args.vg);
			if (isCurrent) {
				// Emphasis ring around the active slot.
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, dx, cy, mm(BeamsLayout::DOTS_R + 0.32f));
				nvgStrokeColor(args.vg, colors[d]);
				nvgStrokeWidth(args.vg, mm(0.25f));
				nvgStroke(args.vg);
			}
		}
	}
};

struct BeamsWidget : ModuleWidget {
	BeamsWidget(Beams* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Beams.svg")));

		using namespace BeamsLayout;
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(LINK_L_X, LINK_L_Y)), module, Beams::LINK_LEFT_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(LINK_R_X, LINK_R_Y)), module, Beams::LINK_RIGHT_LIGHT));

		// All coordinates/sizes come from BeamsLayout.hpp, generated by
		// beams_layout.py alongside the panel SVG, so they cannot drift.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CLOCK_X, IO_Y)), module, Beams::CLOCK_INPUT));
		addParam(placeParam<BeamsKnob>(module, Beams::TEMPO_PARAM, TEMPO_X, IO_Y, TEMPO_SIZE, TEMPO_SIZE));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUTL_X, IO_Y)), module, Beams::MASTER_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUTR_X, IO_Y)), module, Beams::MASTER_R_OUTPUT));

		const NVGcolor brass = nvgRGB(0x8A, 0x64, 0x23);
		const NVGcolor maroon = nvgRGB(0x8A, 0x2A, 0x2A);
		for (int i = 0; i < 4; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TRIG_X[i], TRIG_Y)), module, Beams::TRIG_INPUT + i));

			addParam(placeParam<RRModeButton>(module, Beams::RRMODE_PARAM + i, RR_X[i], RR_Y, RR_W, RR_H));

			auto* dots = new RRDotsWidget();
			dots->module = module;
			dots->track = i;
			dots->box.pos = mm2px(Vec(DOTS_BOX_X[i], DOTS_BOX_Y));
			addChild(dots);

			auto* load = new LoadButton();
			load->module = module;
			load->track = i;
			load->box.pos = mm2px(Vec(LOAD_X[i], LOAD_Y)).minus(load->box.size.div(2.f));
			addChild(load);

			// 2 columns x 3 rows: left = sound, right = rhythm.
			addParam(placeParam<BeamsKnob>(module, Beams::LEVEL_PARAM + i, KNOB_XL[i], KNOB_Y[0], KNOB_SIZE, KNOB_SIZE));
			addParam(placeParam<BeamsKnob>(module, Beams::TUNE_PARAM + i, KNOB_XL[i], KNOB_Y[1], KNOB_SIZE, KNOB_SIZE));
			auto* kDecay = placeParam<BeamsKnob>(module, Beams::DECAY_PARAM + i, KNOB_XL[i], KNOB_Y[2], KNOB_SIZE, KNOB_SIZE);
			kDecay->ringColor = maroon;
			addParam(kDecay);

			auto* kLen = placeParam<BeamsKnob>(module, Beams::LEN_PARAM + i, KNOB_XR[i], KNOB_Y[0], KNOB_SIZE, KNOB_SIZE);
			kLen->ringColor = brass;
			addParam(kLen);
			auto* kSteps = placeParam<BeamsKnob>(module, Beams::STEPS_PARAM + i, KNOB_XR[i], KNOB_Y[1], KNOB_SIZE, KNOB_SIZE);
			kSteps->ringColor = brass;
			addParam(kSteps);
			auto* kRot = placeParam<BeamsKnob>(module, Beams::ROT_PARAM + i, KNOB_XR[i], KNOB_Y[2], KNOB_SIZE, KNOB_SIZE);
			kRot->ringColor = brass;
			addParam(kRot);

			auto* fill = placeParam<FillToggleButton>(module, Beams::FILL_PARAM + i, FILL_X[i], FILL_Y, FILL_W, FILL_H);
			fill->label = "FILL";
			fill->onColor = nvgRGB(0x24, 0x9A, 0x62);   // green
			addParam(fill);
			auto* inv = placeParam<FillToggleButton>(module, Beams::INV_PARAM + i, INV_X[i], FILL_Y, FILL_W, FILL_H);
			inv->label = "INV";
			inv->onColor = nvgRGB(0xE0, 0x7B, 0x1F);    // orange
			addParam(inv);
			auto* mute = placeParam<FillToggleButton>(module, Beams::MUTE_PARAM + i, MUTE_X[i], MUTE_Y, MUTE_W, FILL_H);
			mute->label = "MUTE";
			mute->onColor = nvgRGB(0xC0, 0x39, 0x2B);   // red
			addParam(mute);
		}
	}
};

Model* modelBeams = createModel<Beams, BeamsWidget>("Beams");
