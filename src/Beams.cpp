#include "plugin.hpp"
#include "IntelLink.hpp"
#include "BeamsLayout.hpp"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace rack;

static const int NUM_TRACKS = 4;
static const int MAX_SAMPLES_PER_TRACK = 4;
static const int MAX_EUCLIDEAN_STEPS = 16;

struct SampleBuffer {
	std::vector<float> data;
	int channels = 0;
	int sampleRate = 44100;
	std::string filename;
};

struct LoadedKit {
	std::vector<SampleBuffer> samples;
	std::string folderPath;
};

// In-order Euclidean rhythm generator (Bjorklund's algorithm).
// Produces a bitmask of `steps` length with `pulses` distributed as
// evenly as possible, rotated by `rot` steps to the right.
static uint32_t euclideanPattern(int steps, int pulses, int rot) {
	if (steps <= 0) return 0;
	if (pulses <= 0) return 0;
	if (pulses >= steps) return (1u << steps) - 1u;

	std::vector<std::vector<bool>> sequences(steps);
	for (int i = 0; i < steps; i++) {
		sequences[i].push_back(i < pulses);
	}

	int divisor = steps - pulses;
	int count = pulses;
	while (divisor > 0) {
		int num = std::min(count, divisor);
		for (int i = 0; i < num; i++) {
			sequences[i].insert(sequences[i].end(),
			                    sequences[sequences.size() - num + i].begin(),
			                    sequences[sequences.size() - num + i].end());
		}
		sequences.erase(sequences.end() - num, sequences.end());
		if (count <= divisor) {
			divisor -= count;
		}
		else {
			int newCount = divisor;
			divisor = count - divisor;
			count = newCount;
		}
	}

	std::vector<bool> flat;
	for (const auto& s : sequences) {
		flat.insert(flat.end(), s.begin(), s.end());
	}

	uint32_t mask = 0;
	int len = (int)flat.size();
	for (int i = 0; i < len && i < 32; i++) {
		int srcIdx = (i - rot) % len;
		if (srcIdx < 0) srcIdx += len;
		if (flat[srcIdx]) mask |= (1u << i);
	}
	return mask;
}

struct TrackState {
	float playbackPos = -1.f;
	int currentSampleIdx = 0;
	float envelope = 0.f;
	float activeDecayRate = 0.001f;
	int currentStep = 0;
	dsp::SchmittTrigger trigger;
	float cvPan = 0.f;
};

struct Beams : Module {
	enum ParamId {
		TEMPO_PARAM,
		ENUMS(LEVEL_PARAM, 4),
		ENUMS(LEN_PARAM, 4),
		ENUMS(TUNE_PARAM, 4),
		ENUMS(STEPS_PARAM, 4),
		ENUMS(DECAY_PARAM, 4),
		ENUMS(ROT_PARAM, 4),
		ENUMS(FILL_PARAM, 4),
		ENUMS(INV_PARAM, 4),
		ENUMS(MUTE_PARAM, 4),
		ENUMS(RRMODE_PARAM, 4),
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		ENUMS(TRIG_INPUT, 4),
		INPUTS_LEN
	};
	enum OutputId {
		MASTER_L_OUTPUT,
		MASTER_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LINK_LEFT_LIGHT, LINK_RIGHT_LIGHT,
		ENUMS(TRACK_ACTIVE_LIGHT, 4),
		ENUMS(SAMPLE_LOADED_LIGHT, 4 * 4),
		LIGHTS_LEN
	};

	TrackState tracks[NUM_TRACKS];
	LoadedKit kits[NUM_TRACKS];
	std::string folderToLoad[NUM_TRACKS];
	bool folderLoadPending[NUM_TRACKS] = {};

	dsp::SchmittTrigger clockInTrig;
	dsp::PulseGenerator clockOutPulse;

	float internalPhase = 0.f;

	IntelModMessage leftIntelProducer;
	IntelModMessage leftIntelConsumer;
	IntelModMessage rightIntelProducer;
	IntelModMessage rightIntelConsumer;
	IntelModMessage intelMsg = {};

	Beams() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(TEMPO_PARAM, 30.f, 300.f, 120.f, "Internal Tempo", " BPM");

		for (int i = 0; i < NUM_TRACKS; i++) {
			configParam(LEVEL_PARAM + i, 0.f, 1.f, 0.8f, string::f("Track %d Level", i + 1), "%", 0.f, 100.f);
			configParam(LEN_PARAM + i, 1.f, 16.f, (float)(16 - i * 2), string::f("Track %d Pattern Length", i + 1));
			paramQuantities[LEN_PARAM + i]->snapEnabled = true;

			configParam(TUNE_PARAM + i, -24.f, 24.f, 0.f, string::f("Track %d Tuning", i + 1), " semitones");
			configParam(STEPS_PARAM + i, 0.f, 16.f, (float)(4 + (3 - i) * 2), string::f("Track %d Euclidean Pulses", i + 1));
			paramQuantities[STEPS_PARAM + i]->snapEnabled = true;

			configParam(DECAY_PARAM + i, 0.05f, 4.0f, 0.4f, string::f("Track %d Decay", i + 1), " s");
			configParam(ROT_PARAM + i, 0.f, 15.f, 0.f, string::f("Track %d Pattern Rotation", i + 1), " steps");
			paramQuantities[ROT_PARAM + i]->snapEnabled = true;

			configButton(FILL_PARAM + i, string::f("Track %d Fill (All Pulses)", i + 1));
			configButton(INV_PARAM + i, string::f("Track %d Invert Pattern", i + 1));
			configButton(MUTE_PARAM + i, string::f("Track %d Mute", i + 1));
			configButton(RRMODE_PARAM + i, string::f("Track %d Mode (Seq / Rnd)", i + 1));

			paramQuantities[ROT_PARAM + i]->randomizeEnabled = false;
			paramQuantities[LEVEL_PARAM + i]->randomizeEnabled = false;
			configInput(TRIG_INPUT + i, string::f("Track %d trigger", i + 1));
		}
		configOutput(MASTER_L_OUTPUT, "Master L");
		configOutput(MASTER_R_OUTPUT, "Master R");

		leftExpander.producerMessage = &leftIntelProducer;
		leftExpander.consumerMessage = &leftIntelConsumer;
		rightExpander.producerMessage = &rightIntelProducer;
		rightExpander.consumerMessage = &rightIntelConsumer;
	}

	static bool isFamilyModule(Module* m) {
		if (!m || !m->model || !m->model->plugin) return false;
		const std::string& pslug = m->model->plugin->slug;
		const std::string& mslug = m->model->slug;
		return (
			mslug == "SpacesCommand" || pslug == "SpacesCommand" || pslug == "Grandsummoner-SpacesCommand" ||
			mslug == "Intel"         || pslug == "Intel"         || pslug == "Grandsummoner-Intel" ||
			mslug == "Beams"         || pslug == "Beams"         || pslug == "Grandsummoner-Beams" ||
			mslug == "Stellar"       || pslug == "Stellar"       || pslug == "Grandsummoner-Stellar" ||
			mslug == "Explorer"      || pslug == "Explorer"      || pslug == "Grandsummoner-Explorer" ||
			pslug == "HermeticModular"
		);
	}

	std::mutex sampleMutex;

	bool loadTrackFolder(int track, const std::string& dir, float engineRate) {
		LoadedKit kit;
		kit.folderPath = dir;

		std::vector<std::string> wavFiles;
		DIR* dp = opendir(dir.c_str());
		if (!dp) return false;

		struct dirent* ep;
		while ((ep = readdir(dp))) {
			std::string name = ep->d_name;
			if (name.size() > 4) {
				std::string ext = name.substr(name.size() - 4);
				std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
				if (ext == ".wav") {
					wavFiles.push_back(dir + "/" + name);
				}
			}
		}
		closedir(dp);
		std::sort(wavFiles.begin(), wavFiles.end());

		int count = std::min((int)wavFiles.size(), MAX_SAMPLES_PER_TRACK);
		for (int i = 0; i < count; i++) {
			drwav wav;
			if (!drwav_init_file(&wav, wavFiles[i].c_str(), nullptr)) continue;

			SampleBuffer sb;
			sb.channels = wav.channels;
			sb.sampleRate = wav.sampleRate;
			sb.filename = wavFiles[i].substr(wavFiles[i].find_last_of("/\\") + 1);

			size_t totalSamples = wav.totalPCMFrameCount * wav.channels;
			sb.data.resize(totalSamples);
			drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, sb.data.data());
			drwav_uninit(&wav);

			kit.samples.push_back(std::move(sb));
		}

		if (kit.samples.empty()) return false;

		{
			std::lock_guard<std::mutex> lock(sampleMutex);
			kits[track] = std::move(kit);
			tracks[track].playbackPos = -1.f;
			tracks[track].currentSampleIdx = 0;
		}
		return true;
	}

	void triggerTrack(int t, float sampleRate) {
		TrackState& tr = tracks[t];
		const LoadedKit& kit = kits[t];
		if (kit.samples.empty()) return;

		bool randomMode = params[RRMODE_PARAM + t].getValue() > 0.5f;
		if (randomMode) {
			tr.currentSampleIdx = rand() % kit.samples.size();
		}
		else {
			tr.currentSampleIdx = (tr.currentSampleIdx + 1) % kit.samples.size();
		}
		tr.playbackPos = 0.f;
		tr.envelope = 1.f;

		float decaySec = params[DECAY_PARAM + t].getValue();
		if (intelMsg.type == INTEL_LINK_MOD_MESSAGE && intelMsg.tone > 0.001f) {
			decaySec *= (0.5f + intelMsg.tone * 1.5f);
		}
		tr.activeDecayRate = 1.f / (decaySec * sampleRate);
	}

	void process(const ProcessArgs& args) override {
		for (int t = 0; t < NUM_TRACKS; t++) {
			if (folderLoadPending[t]) {
				folderLoadPending[t] = false;
				loadTrackFolder(t, folderToLoad[t], args.sampleRate);
			}
		}

		bool chainLeft = (bool) intelLinkFind(this, false, "Intel");
		bool chainRight = (bool) intelLinkFind(this, true, "Intel");
		if (chainLeft) intelMsg = *(IntelModMessage*)leftExpander.consumerMessage;
		else if (chainRight) intelMsg = *(IntelModMessage*)rightExpander.consumerMessage;
		else intelMsg = {};

		lights[LINK_LEFT_LIGHT].setBrightness(isFamilyModule(leftExpander.module) ? 1.f : 0.f);
		lights[LINK_RIGHT_LIGHT].setBrightness(isFamilyModule(rightExpander.module) ? 1.f : 0.f);

		bool clockPatched = inputs[CLOCK_INPUT].isConnected();
		bool pulse = false;
		if (clockPatched) {
			pulse = clockInTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f);
		}
		else {
			float bpm = params[TEMPO_PARAM].getValue();
			if (intelMsg.type == INTEL_LINK_MOD_MESSAGE && intelMsg.rate > 0.001f) {
				bpm = 60.f + intelMsg.rate * 180.f;
			}
			float freq = (bpm / 60.f) * 4.f;
			internalPhase += freq * args.sampleTime;
			if (internalPhase >= 1.f) {
				internalPhase -= 1.f;
				pulse = true;
			}
		}

		uint8_t firedMask = 0;

		for (int t = 0; t < NUM_TRACKS; t++) {
			TrackState& tr = tracks[t];
			bool manualTrig = inputs[TRIG_INPUT + t].isConnected() &&
			                  tr.trigger.process(inputs[TRIG_INPUT + t].getVoltage(), 0.1f, 2.f);

			if (pulse) {
				int len = (int)params[LEN_PARAM + t].getValue();
				int pulses = (int)params[STEPS_PARAM + t].getValue();
				int rot = (int)params[ROT_PARAM + t].getValue();
				bool fill = params[FILL_PARAM + t].getValue() > 0.5f;
				bool inv  = params[INV_PARAM + t].getValue() > 0.5f;
				bool mute = params[MUTE_PARAM + t].getValue() > 0.5f;

				uint32_t pat = euclideanPattern(len, pulses, rot);
				if (fill) pat = (1u << len) - 1u;
				if (inv)  pat = (~pat) & ((1u << len) - 1u);

				bool stepHit = (pat & (1u << (tr.currentStep % len))) != 0;
				tr.currentStep = (tr.currentStep + 1) % len;

				if (stepHit && !mute) {
					triggerTrack(t, args.sampleRate);
					firedMask |= (1u << t);
				}
			}

			if (manualTrig && !(params[MUTE_PARAM + t].getValue() > 0.5f)) {
				triggerTrack(t, args.sampleRate);
				firedMask |= (1u << t);
			}
		}

		if (pulse) {
			Module* intelL = intelLinkFind(this, false, "Intel");
			Module* intelR = intelLinkFind(this, true, "Intel");
			BeamsTickMessage tmsg;
			tmsg.type = INTEL_LINK_BEAMS_TICK;
			tmsg.tickCount = 0;
			tmsg.tempoBpm = params[TEMPO_PARAM].getValue();
			tmsg.activeTrackMask = firedMask;
			if (intelL) {
				*(IntelModMessage*)intelL->rightExpander.producerMessage = *(IntelModMessage*)(void*)&tmsg;
				intelL->rightExpander.messageFlipRequested = true;
			}
			if (intelR) {
				*(IntelModMessage*)intelR->leftExpander.producerMessage = *(IntelModMessage*)(void*)&tmsg;
				intelR->leftExpander.messageFlipRequested = true;
			}
		}

		float mixL = 0.f;
		float mixR = 0.f;

		if (sampleMutex.try_lock()) {
			for (int t = 0; t < NUM_TRACKS; t++) {
				TrackState& tr = tracks[t];
				const LoadedKit& kit = kits[t];

				if (tr.playbackPos >= 0.f && tr.currentSampleIdx < (int)kit.samples.size()) {
					const SampleBuffer& sb = kit.samples[tr.currentSampleIdx];
					if (!sb.data.empty()) {
						float tune = params[TUNE_PARAM + t].getValue();
						float speed = std::pow(2.f, tune / 12.f) * ((float)sb.sampleRate / args.sampleRate);

						int idx = (int)tr.playbackPos;
						float frac = tr.playbackPos - idx;

						float sL = 0.f, sR = 0.f;
						if (sb.channels == 1) {
							if (idx + 1 < (int)sb.data.size()) {
								sL = sR = sb.data[idx] * (1.f - frac) + sb.data[idx + 1] * frac;
							}
						}
						else if (sb.channels == 2) {
							int frame = idx * 2;
							if (frame + 3 < (int)sb.data.size()) {
								sL = sb.data[frame] * (1.f - frac) + sb.data[frame + 2] * frac;
								sR = sb.data[frame + 1] * (1.f - frac) + sb.data[frame + 3] * frac;
							}
						}

						float gain = params[LEVEL_PARAM + t].getValue() * tr.envelope;
						mixL += sL * gain;
						mixR += sR * gain;

						tr.playbackPos += speed;
						tr.envelope -= tr.activeDecayRate;
						if (tr.envelope <= 0.f || (int)tr.playbackPos >= (int)(sb.data.size() / sb.channels)) {
							tr.playbackPos = -1.f;
							tr.envelope = 0.f;
						}
					}
				}

				lights[TRACK_ACTIVE_LIGHT + t].setBrightness(tr.envelope);
				for (int s = 0; s < MAX_SAMPLES_PER_TRACK; s++) {
					bool has = s < (int)kit.samples.size();
					bool active = has && (s == tr.currentSampleIdx) && (tr.envelope > 0.05f);
					lights[SAMPLE_LOADED_LIGHT + t * MAX_SAMPLES_PER_TRACK + s].setBrightness(active ? 1.f : (has ? 0.25f : 0.f));
				}
			}
			sampleMutex.unlock();
		}

		outputs[MASTER_L_OUTPUT].setVoltage(clamp(mixL * 5.f, -10.f, 10.f));
		outputs[MASTER_R_OUTPUT].setVoltage(clamp(mixR * 5.f, -10.f, 10.f));
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_t* kitsJ = json_array();
		for (int t = 0; t < NUM_TRACKS; t++) {
			json_array_append_new(kitsJ, json_string(kits[t].folderPath.c_str()));
		}
		json_object_set_new(rootJ, "kits", kitsJ);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* kitsJ = json_object_get(rootJ, "kits");
		if (kitsJ) {
			for (int t = 0; t < NUM_TRACKS; t++) {
				json_t* fJ = json_array_get(kitsJ, t);
				if (fJ && json_is_string(fJ)) {
					std::string path = json_string_value(fJ);
					if (!path.empty()) {
						folderToLoad[t] = path;
						folderLoadPending[t] = true;
					}
				}
			}
		}
	}
};

static inline float mm(float v) {
	return v * 72.f / 25.4f;
}

static void drawBoldText(NVGcontext* vg, float cx, float cy, const char* str, float sizePt, NVGcolor color) {
	nvgSave(vg);
	std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans-Bold.ttf"));
	if (font) nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, sizePt);
	nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, color);
	nvgText(vg, cx, cy, str, nullptr);
	nvgRestore(vg);
}

template <typename T>
static T* placeParam(Module* module, int paramId, float x_mm, float y_mm, float w_mm, float h_mm) {
	T* w = createParam<T>(Vec(), module, paramId);
	w->box.size = mm2px(Vec(w_mm, h_mm));
	w->box.pos  = mm2px(Vec(x_mm, y_mm));
	return w;
}

template <typename T>
static T* placeCentered(Module* module, int paramId, float cx_mm, float cy_mm, float w_mm, float h_mm) {
	T* w = createParam<T>(Vec(), module, paramId);
	w->box.size = mm2px(Vec(w_mm, h_mm));
	w->box.pos  = mm2px(Vec(cx_mm - w_mm * 0.5f, cy_mm - h_mm * 0.5f));
	return w;
}

struct BeamsKnob : RoundKnob {
	NVGcolor ringColor = nvgRGB(0x8A, 0x64, 0x23);

	BeamsKnob() {
		box.size = Vec(mm(BeamsLayout::KNOB_SIZE), mm(BeamsLayout::KNOB_SIZE));
	}

	void draw(const DrawArgs& args) override {
		float r = box.size.x * 0.5f;
		float cx = r, cy = r;

		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, r - 0.4f);
		nvgFillColor(args.vg, nvgRGB(0x2E, 0x28, 0x1F));
		nvgFill(args.vg);

		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, r - 0.6f);
		nvgStrokeColor(args.vg, ringColor);
		nvgStrokeWidth(args.vg, 0.75f);
		nvgStroke(args.vg);

		float value = 0.5f;
		ParamQuantity* pq = getParamQuantity();
		if (pq) value = pq->getScaledValue();
		float angle = rescale(value, 0.f, 1.f, -0.8f * M_PI, 0.8f * M_PI) - M_PI / 2.f;

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, cx, cy);
		nvgLineTo(args.vg, cx + std::cos(angle) * (r - 1.2f), cy + std::sin(angle) * (r - 1.2f));
		nvgStrokeColor(args.vg, nvgRGB(0xED, 0xE7, 0xDC));
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
	}

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			ParamQuantity* pq = getParamQuantity();
			if (pq) pq->setValue(pq->getDefaultValue());
			e.consume(this);
			return;
		}
		ParamWidget::onButton(e);
	}
};

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
		nvgFillColor(args.vg, rnd ? nvgRGB(0x2E, 0x4A, 0x6E) : nvgRGB(0xED, 0xE7, 0xDC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, rnd ? nvgRGB(0x2E, 0x4A, 0x6E) : nvgRGB(0x4A, 0x44, 0x38));
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

struct LoadButton : OpaqueWidget {
	int track = 0;
	Beams* module = nullptr;

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
		drawBoldText(args.vg, box.size.x / 2.f, box.size.y / 2.f, "LOAD", 2.2f, nvgRGB(0x2A, 0x26, 0x20));
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (module) {
				std::string dir = os::dialog::selectDirectory();
				if (!dir.empty()) {
					module->folderToLoad[track] = dir;
					module->folderLoadPending[track] = true;
				}
			}
			e.consume(this);
		}
	}
};

struct BeamsWidget : ModuleWidget {
	BeamsWidget(Beams* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Beams.svg")));

		using namespace BeamsLayout;

		NVGcolor navy  = nvgRGB(0x2E, 0x4A, 0x6E);
		NVGcolor brass = nvgRGB(0x8A, 0x64, 0x23);

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JACK_CLOCK_X, JACK_TOP_Y)), module, Beams::CLOCK_INPUT));
		addParam(placeCentered<BeamsKnob>(module, Beams::TEMPO_PARAM, TEMPO_X, TEMPO_Y, TEMPO_SIZE, TEMPO_SIZE));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(JACK_OUTL_X, JACK_TOP_Y)), module, Beams::MASTER_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(JACK_OUTR_X, JACK_TOP_Y)), module, Beams::MASTER_R_OUTPUT));

		for (int i = 0; i < NUM_TRACKS; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TRIG_X[i], TRIG_Y)), module, Beams::TRIG_INPUT + i));
		}

		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(LINK_L_X, LINK_L_Y)), module, Beams::LINK_LEFT_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(LINK_R_X, LINK_R_Y)), module, Beams::LINK_RIGHT_LIGHT));

		for (int i = 0; i < NUM_TRACKS; i++) {
			addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(ACT_X[i], ACT_Y)), module, Beams::TRACK_ACTIVE_LIGHT + i));

			for (int s = 0; s < MAX_SAMPLES_PER_TRACK; s++) {
				addChild(createLightCentered<SmallLight<YellowLight>>(
					mm2px(Vec(PIPS_X[i][s], PIPS_Y)),
					module,
					Beams::SAMPLE_LOADED_LIGHT + i * MAX_SAMPLES_PER_TRACK + s));
			}

			addParam(placeParam<RRModeButton>(module, Beams::RRMODE_PARAM + i, RR_X[i], RR_Y, RR_W, RR_H));

			auto* load = new LoadButton();
			load->track = i;
			load->module = module;
			load->box.pos = mm2px(Vec(LOAD_X[i], LOAD_Y));
			addChild(load);

			auto* kLevel = placeParam<BeamsKnob>(module, Beams::LEVEL_PARAM + i, KNOB_XL[i], KNOB_Y[0], KNOB_SIZE, KNOB_SIZE);
			kLevel->ringColor = navy;
			addParam(kLevel);
			auto* kTune = placeParam<BeamsKnob>(module, Beams::TUNE_PARAM + i, KNOB_XL[i], KNOB_Y[1], KNOB_SIZE, KNOB_SIZE);
			kTune->ringColor = navy;
			addParam(kTune);
			auto* kDecay = placeParam<BeamsKnob>(module, Beams::DECAY_PARAM + i, KNOB_XL[i], KNOB_Y[2], KNOB_SIZE, KNOB_SIZE);
			kDecay->ringColor = navy;
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
			mute->onColor = nvgRGB(0x8A, 0x2A, 0x2A);   // maroon (Family Freeze / Mute / Active)
			addParam(mute);
		}
	}
};

Model* modelBeams = createModel<Beams, BeamsWidget>("Beams");
