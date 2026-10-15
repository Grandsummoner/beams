#include "plugin.hpp"
#include "IntelLink.hpp"
#include <osdialog.h>
#include <system.hpp>
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include <algorithm>
#include <vector>
#include <string>

// ---------------------------------------------------------------------
// Euclidean rhythm generator
// ---------------------------------------------------------------------
// Bresenham-line distribution -- a simple, well-established practical
// equivalent to Bjorklund's recursive algorithm for spreading `k` hits
// as evenly as possible across `n` steps (same result class used by a
// lot of hardware Euclidean implementations). Far less code than a true
// recursive Bjorklund, with no meaningful difference for this use.
static std::vector<bool> euclideanPattern(int n, int k) {
	std::vector<bool> pattern(std::max(n, 1), false);
	if (n <= 0) return pattern;
	k = clamp(k, 0, n);
	if (k <= 0) return pattern;
	if (k >= n) { pattern.assign(n, true); return pattern; }
	int bucket = 0;
	for (int i = 0; i < n; i++) {
		bucket += k;
		if (bucket >= n) {
			bucket -= n;
			pattern[i] = true;
		}
	}
	return pattern;
}

// ---------------------------------------------------------------------
// Per-track sample bank + voice state
// ---------------------------------------------------------------------
struct SampleSlot {
	std::vector<float> data;  // mono, already resampled to the engine's rate
	bool loaded = false;
};

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
	dsp::BooleanTrigger rrModeTrig, fillTrig, invTrig;
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

	// Loads up to 4 .wav files (alphabetical) from `dir` into this
	// track's slots, resampling each to `engineRate` with simple linear
	// interpolation (adequate for a one-time load-time operation; the
	// TUNE knob already does runtime linear-interpolated playback, so
	// this matches that same quality tier rather than pulling in a
	// second resampling method).
	bool loadFolder(const std::string& dir, float engineRate) {
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

		for (int i = 0; i < 4; i++) slots[i] = SampleSlot();
		int count = 0;
		for (auto& path : wavs) {
			unsigned int channels = 0, sampleRate = 0;
			drwav_uint64 totalFrames = 0;
			float* raw = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &sampleRate, &totalFrames, NULL);
			if (!raw) continue;

			// Fold to mono.
			std::vector<float> mono(totalFrames);
			for (drwav_uint64 f = 0; f < totalFrames; f++) {
				float sum = 0.f;
				for (unsigned int c = 0; c < channels; c++) sum += raw[f * channels + c];
				mono[f] = sum / (float) channels;
			}
			drwav_free(raw, NULL);

			// Resample to engine rate if needed (simple linear interp --
			// fine for a one-shot load, not a real-time path).
			SampleSlot& slot = slots[count];
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
		loadedCount = count;
		rrIndex = 0;
		lastPlayed = -1;
		return count > 0;
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

	// Intel link -- same buffer-ownership convention as Command's own
	// copy: allocated here, not by Intel, since the sending convention
	// is "write into the NEIGHBOR's own producer buffer".
	IntelModMessage leftIntelProducer, leftIntelConsumer, rightIntelProducer, rightIntelConsumer;
	bool intelPresent = false;
	float intelDecayOffset = 0.f;

	Beams() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TEMPO_PARAM, 20.f, 300.f, 120.f, "Tempo (used only when CLOCK is unpatched)", " BPM");
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

	void onReset() override {
		for (int i = 0; i < 4; i++) tracks[i] = TrackVoice();
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

		float mixL = 0.f, mixR = 0.f;
		for (int i = 0; i < 4; i++) {
			TrackVoice& t = tracks[i];

			// --- Toggles (momentary params, edge-detected to flip state) ---
			if (t.rrModeTrig.process(params[RRMODE_PARAM + i].getValue() > 0.5f)) t.rrMode = !t.rrMode;
			if (t.fillTrig.process(params[FILL_PARAM + i].getValue() > 0.5f)) t.fillOn = !t.fillOn;
			if (t.invTrig.process(params[INV_PARAM + i].getValue() > 0.5f)) t.invOn = !t.invOn;

			// --- Triggering: external TRIG input overrides internal Euclidean ---
			bool patched = inputs[TRIG_INPUT + i].isConnected();
			if (patched) {
				float v = inputs[TRIG_INPUT + i].getVoltage();
				if (t.trigInTrig.process(v, 0.1f, 2.f)) {
					float vel = clamp(v / 10.f, 0.1f, 1.f);
					t.trigger(vel);
				}
			}
			else if (pulse) {
				int len = (int) std::round(params[LEN_PARAM + i].getValue());
				int steps = (int) std::round(params[STEPS_PARAM + i].getValue());
				int rot = (int) std::round(params[ROT_PARAM + i].getValue());
				t.stepCounter = (t.stepCounter + 1) % len;
				if (t.stepHit(len, steps, rot)) t.trigger(1.f);
			}

			// --- Playback: resampled (TUNE) mono read, attack+decay envelope ---
			float voice = 0.f;
			if (t.activeSlot >= 0 && t.loadedCount > 0) {
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

// Generic rotary knob, fully custom-drawn (no SVG knob asset) -- same
// approach as StellarKnob/MorphKnob/IntelKnob elsewhere in the family.
// One reusable class covers both TEMPO (bigger radius) and the dense
// per-track knobs (smaller radius), sized per-instance via box.size.
struct BeamsKnob : ParamWidget {
	NVGcolor ringColor = nvgRGB(0x4A, 0x44, 0x38);

	void draw(const DrawArgs& args) override {
		float r = box.size.x / 2.f;
		float cx = r, cy = r;
		ParamQuantity* pq = getParamQuantity();
		float value = pq ? pq->getValue() : 0.f;
		float minV = pq ? pq->getMinValue() : 0.f;
		float maxV = pq ? pq->getMaxValue() : 1.f;
		float norm = (maxV > minV) ? clamp((value - minV) / (maxV - minV), 0.f, 1.f) : 0.f;

		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, r * 0.82f);
		nvgFillColor(args.vg, nvgRGB(0xED, 0xE7, 0xDC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, ringColor);
		nvgStrokeWidth(args.vg, r * 0.14f);
		nvgStroke(args.vg);

		// Pointer -- same -0.75pi..+0.75pi convention as the rest of the
		// family's knobs (MorphKnob/Trimpot/etc).
		float angle = -0.75f * (float)M_PI + norm * 1.5f * (float)M_PI;
		float px = cx + std::sin(angle) * r * 0.75f;
		float py = cy - std::cos(angle) * r * 0.75f;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, cx, cy);
		nvgLineTo(args.vg, px, py);
		nvgStrokeColor(args.vg, nvgRGB(0x2A, 0x26, 0x20));
		nvgStrokeWidth(args.vg, r * 0.18f);
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

// SEQ/RND toggle -- the one toggle that needs to draw its own dynamic
// text (the static panel guide just says "SEQ" always), same font-glyph
// drawing convention as Command's SquareButton.
struct RRModeButton : ParamWidget {
	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 1.0f);
		nvgFillColor(args.vg, nvgRGB(0xED, 0xE7, 0xDC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x4A, 0x44, 0x38));
		nvgStrokeWidth(args.vg, 0.4f * box.size.y / 3.8f);
		nvgStroke(args.vg);

		ParamQuantity* pq = getParamQuantity();
		bool rnd = pq && pq->getValue() > 0.5f;
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (font && font->handle >= 0) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, box.size.y * 0.5f);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(args.vg, nvgRGB(0x2A, 0x26, 0x20));
			nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f + 0.3f, rnd ? "RND" : "SEQ", NULL);
		}
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			ParamQuantity* pq = getParamQuantity();
			if (pq) pq->setValue(pq->getValue() > 0.5f ? 0.f : 1.f);
			e.consume(this);
		}
	}
};

// FILL / INV toggles -- the panel's own static text already labels
// these, so the widget only needs to show active/inactive state via a
// filled background, no text of its own.
struct FillToggleButton : ParamWidget {
	NVGcolor onColor = nvgRGB(0x8A, 0x64, 0x23);

	void draw(const DrawArgs& args) override {
		ParamQuantity* pq = getParamQuantity();
		bool on = pq && pq->getValue() > 0.5f;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 0.8f);
		nvgFillColor(args.vg, on ? onColor : nvgRGB(0xED, 0xE7, 0xDC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x4A, 0x44, 0x38));
		nvgStrokeWidth(args.vg, 0.35f);
		nvgStroke(args.vg);
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

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 1.2f);
		nvgFillColor(args.vg, nvgRGB(0xED, 0xE7, 0xDC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x4A, 0x44, 0x38));
		nvgStrokeWidth(args.vg, 0.4f);
		nvgStroke(args.vg);
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (font && font->handle >= 0) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, box.size.y * 0.42f);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(args.vg, nvgRGB(0x4A, 0x44, 0x38));
			nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f, "LOAD", NULL);
		}
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			e.consume(this);
			if (!module) return;
			char* path = osdialog_file(OSDIALOG_OPEN_DIR, NULL, NULL, NULL);
			if (path) {
				module->tracks[track].loadFolder(path, APP->engine->getSampleRate());
				free(path);
			}
		}
	}
};

// Round-robin dot readout -- 4 dots, fixed identity colors (reusing
// Intel's FX quartet), filled when that slot has a sample loaded.
struct RRDotsWidget : Widget {
	Beams* module = nullptr;
	int track = 0;
	float pitch = 2.33f;
	static constexpr int N = 4;
	NVGcolor colors[N] = {
		nvgRGB(0x8A, 0x64, 0x23), nvgRGB(0x8A, 0x2A, 0x2A),
		nvgRGB(0x2E, 0x4A, 0x6E), nvgRGB(0x6B, 0x4C, 0x8A),
	};

	void draw(const DrawArgs& args) override {
		int loaded = module ? module->tracks[track].loadedCount : 0;
		float cy = box.size.y / 2.f;
		float x0 = box.size.x / 2.f - pitch * 1.5f;
		for (int d = 0; d < N; d++) {
			float dx = x0 + d * pitch;
			bool lit = d < loaded;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, dx, cy, 0.6f);
			if (lit) {
				nvgFillColor(args.vg, colors[d]);
				nvgFill(args.vg);
			}
			nvgStrokeColor(args.vg, colors[d]);
			nvgStrokeWidth(args.vg, 0.35f);
			nvgStroke(args.vg);
		}
	}
};

struct BeamsWidget : ModuleWidget {
	BeamsWidget(Beams* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Beams.svg")));

		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(5.5, 5.5)), module, Beams::LINK_LEFT_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(85.94, 5.5)), module, Beams::LINK_RIGHT_LIGHT));

		// --- I/O row ---
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.67, 18.3)), module, Beams::CLOCK_INPUT));
		{
			auto* k = createParamCentered<BeamsKnob>(mm2px(Vec(39.43, 18.3)), module, Beams::TEMPO_PARAM);
			k->box.size = mm2px(Vec(7.2, 7.2));
			addParam(k);
		}
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(56.2, 18.3)), module, Beams::MASTER_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(73.77, 18.3)), module, Beams::MASTER_R_OUTPUT));

		// --- Per-track controls -- coordinates match beams_layout.json
		// exactly (generated by beams_layout.py), so the widgets and the
		// printed panel can't drift apart from each other. ---
		static const float trigX[4] = {19.18f, 38.54f, 57.90f, 77.26f};
		static const float trigY = 40.11f;
		static const float toggleX[4] = {13.58f, 32.94f, 52.30f, 71.66f};
		static const float dotsX[4] = {19.27f, 38.63f, 57.99f, 77.35f};
		static const float rowY = 53.51f;
		static const float loadX[4] = {19.18f, 38.54f, 57.90f, 77.26f};
		static const float loadY = 64.51f;
		static const float knobRow1Y = 75.11f;
		static const float knobRow2Y = 85.81f;
		static const float knobXLo[4] = {13.00f, 32.36f, 51.72f, 71.08f};
		static const float knobXMid[4] = {19.18f, 38.54f, 57.90f, 77.26f};
		static const float knobXHi[4] = {25.36f, 44.72f, 64.08f, 83.44f};
		static const float fillX[4] = {14.64f, 34.00f, 53.36f, 72.72f};
		static const float invX[4] = {23.72f, 43.08f, 62.44f, 81.80f};
		static const float fillY = 96.71f;
		static const int levelP[4] = {Beams::LEVEL_PARAM + 0, Beams::LEVEL_PARAM + 1, Beams::LEVEL_PARAM + 2, Beams::LEVEL_PARAM + 3};
		static const int tuneP[4] = {Beams::TUNE_PARAM + 0, Beams::TUNE_PARAM + 1, Beams::TUNE_PARAM + 2, Beams::TUNE_PARAM + 3};
		static const int decayP[4] = {Beams::DECAY_PARAM + 0, Beams::DECAY_PARAM + 1, Beams::DECAY_PARAM + 2, Beams::DECAY_PARAM + 3};
		static const int lenP[4] = {Beams::LEN_PARAM + 0, Beams::LEN_PARAM + 1, Beams::LEN_PARAM + 2, Beams::LEN_PARAM + 3};
		static const int stepsP[4] = {Beams::STEPS_PARAM + 0, Beams::STEPS_PARAM + 1, Beams::STEPS_PARAM + 2, Beams::STEPS_PARAM + 3};
		static const int rotP[4] = {Beams::ROT_PARAM + 0, Beams::ROT_PARAM + 1, Beams::ROT_PARAM + 2, Beams::ROT_PARAM + 3};

		for (int i = 0; i < 4; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(trigX[i], trigY)), module, Beams::TRIG_INPUT + i));

			auto* rr = createParamCentered<RRModeButton>(mm2px(Vec(toggleX[i], rowY)), module, Beams::RRMODE_PARAM + i);
			rr->box.size = mm2px(Vec(9.0, 3.8));
			addParam(rr);

			auto* dots = new RRDotsWidget();
			dots->module = module;
			dots->track = i;
			dots->box.size = mm2px(Vec(10.6, 5.2));
			dots->box.pos = mm2px(Vec(dotsX[i], rowY)).minus(dots->box.size.div(2));
			addChild(dots);

			auto* load = new LoadButton();
			load->module = module;
			load->track = i;
			load->box.size = mm2px(Vec(14.9, 4.2));
			load->box.pos = mm2px(Vec(loadX[i], loadY)).minus(load->box.size.div(2));
			addChild(load);

			auto* kLevel = createParamCentered<BeamsKnob>(mm2px(Vec(knobXLo[i], knobRow1Y)), module, levelP[i]);
			kLevel->box.size = mm2px(Vec(3.6, 3.6));
			addParam(kLevel);
			auto* kTune = createParamCentered<BeamsKnob>(mm2px(Vec(knobXMid[i], knobRow1Y)), module, tuneP[i]);
			kTune->box.size = mm2px(Vec(3.6, 3.6));
			addParam(kTune);
			auto* kDecay = createParamCentered<BeamsKnob>(mm2px(Vec(knobXHi[i], knobRow1Y)), module, decayP[i]);
			kDecay->box.size = mm2px(Vec(3.6, 3.6));
			kDecay->ringColor = nvgRGB(0x8A, 0x2A, 0x2A);
			addParam(kDecay);

			auto* kLen = createParamCentered<BeamsKnob>(mm2px(Vec(knobXLo[i], knobRow2Y)), module, lenP[i]);
			kLen->box.size = mm2px(Vec(3.6, 3.6));
			kLen->ringColor = nvgRGB(0x8A, 0x64, 0x23);
			addParam(kLen);
			auto* kSteps = createParamCentered<BeamsKnob>(mm2px(Vec(knobXMid[i], knobRow2Y)), module, stepsP[i]);
			kSteps->box.size = mm2px(Vec(3.6, 3.6));
			kSteps->ringColor = nvgRGB(0x8A, 0x64, 0x23);
			addParam(kSteps);
			auto* kRot = createParamCentered<BeamsKnob>(mm2px(Vec(knobXHi[i], knobRow2Y)), module, rotP[i]);
			kRot->box.size = mm2px(Vec(3.6, 3.6));
			kRot->ringColor = nvgRGB(0x8A, 0x64, 0x23);
			addParam(kRot);

			auto* fill = createParamCentered<FillToggleButton>(mm2px(Vec(fillX[i], fillY)), module, Beams::FILL_PARAM + i);
			fill->box.size = mm2px(Vec(4.2, 3.6));
			addParam(fill);
			auto* inv = createParamCentered<FillToggleButton>(mm2px(Vec(invX[i], fillY)), module, Beams::INV_PARAM + i);
			inv->box.size = mm2px(Vec(4.2, 3.6));
			inv->onColor = nvgRGB(0x8A, 0x2A, 0x2A);
			addParam(inv);
		}
	}
};

Model* modelBeams = createModel<Beams, BeamsWidget>("Beams");
