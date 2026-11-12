// TunedPerc.cpp
// Plays one percussion sample. When the sample is loaded, its pitch is
// detected (see PercEngine.hpp) and every hit is shifted so the drum sits on
// the nearest C. V/OCT then moves it like any oscillator, so the drum plays in
// key with the rest of the track: 0 V = the drum's own C.
#include "plugin.hpp"
#include "PercEngine.hpp"
#include <osdialog.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <cctype>

using namespace tp;

// A loaded sample with its analysis. Never changed after it is published.
struct Loaded {
	std::string path;
	Sample sample;
	PitchResult pitch;
	bool tuned() const { return pitch.found && pitch.confidence >= MIN_CONFIDENCE; }
	// the note the sample sounds at before any knobs (60 if unknown)
	float baseNote() const { return pitch.found ? pitch.note : 60.f; }
};

struct OctaveQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		int o = (int) std::round(getValue());
		return o == 0 ? "0" : string::f("%+d octave%s", o, std::abs(o) == 1 ? "" : "s");
	}
};

struct DecayQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = getValue();
		if (v >= 0.99f) return "whole sample";
		float s = PercEngine::decaySeconds(v);
		return s < 1.f ? string::f("%.0f ms", s * 1000.f) : string::f("%.2f s", s);
	}
};

struct LevelQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float g = 2.f * getValue() * getValue();
		if (g < 1e-4f) return "-inf dB";
		return string::f("%+.1f dB", 20.f * std::log10(g));
	}
};

struct TunedPerc : Module {
	enum ParamId { OCTAVE_PARAM, FINE_PARAM, DECAY_PARAM, LEVEL_PARAM, TUNE_PARAM, TRACK_PARAM, DETECT_PARAM, PARAMS_LEN };
	enum InputId { TRIG_INPUT, VOCT_INPUT, VEL_INPUT, INPUTS_LEN };
	enum OutputId { OUT_OUTPUT, ENV_OUTPUT, NOTE_OUTPUT, OUTPUTS_LEN };
	enum LightId { DETECT_LIGHT, LIGHTS_LEN };

	PercEngine engine;
	// Loaded samples are built on a loader thread and published in uiLoaded
	// (std::atomic_load/atomic_store). The audio thread takes its own copy
	// when the version changes.
	std::shared_ptr<Loaded> uiLoaded;
	std::atomic<int> version{0};
	std::shared_ptr<Loaded> audioLoaded;   // audio thread only
	int audioVersion = 0;

	std::thread loader;
	std::atomic<bool> busy{false};
	std::mutex textMutex;
	std::string path, message;
	PitchFocus focus = FOCUS_SETTLED;

	dsp::SchmittTrigger trigIn;
	std::atomic<float> playingNote{-1000.f};   // for the display
	float lastSemis = 0.f;

	TunedPerc() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<OctaveQuantity>(OCTAVE_PARAM, -3.f, 3.f, 0.f, "Octave");
		getParamQuantity(OCTAVE_PARAM)->snapEnabled = true;
		configParam(FINE_PARAM, -50.f, 50.f, 0.f, "Fine", " cents");
		configParam<DecayQuantity>(DECAY_PARAM, 0.f, 1.f, 1.f, "Decay");
		configParam<LevelQuantity>(LEVEL_PARAM, 0.f, 1.f, 0.7071f, "Level");
		configSwitch(TUNE_PARAM, 0.f, 1.f, 1.f, "Tune", {"As is (no tuning)", "To C"});
		configSwitch(TRACK_PARAM, 0.f, 1.f, 0.f, "Pitch", {"Hold (V/OCT taken when each hit starts)", "Track (follows V/OCT during the hit)"});
		configButton(DETECT_PARAM, "Detect the pitch again");
		configInput(TRIG_INPUT, "Trigger");
		configInput(VOCT_INPUT, "V/OCT (0 V = the drum's own C)");
		configInput(VEL_INPUT, "Velocity (0-10 V, normalled to 10 V)");
		configOutput(OUT_OUTPUT, "Audio");
		configOutput(ENV_OUTPUT, "Envelope (0-10 V)");
		configOutput(NOTE_OUTPUT, "Note playing (V/OCT, 0 V = C4)");
	}

	~TunedPerc() {
		if (loader.joinable()) loader.join();
	}

	std::string getPath() {
		std::lock_guard<std::mutex> lock(textMutex);
		return path;
	}
	std::string getMessage() {
		std::lock_guard<std::mutex> lock(textMutex);
		return message;
	}
	void setText(const std::string& p, const std::string& m) {
		std::lock_guard<std::mutex> lock(textMutex);
		path = p;
		message = m;
	}

	// semitones from the sample as recorded, without V/OCT
	float knobSemis(const Loaded* l) {
		float s = 0.f;
		if (l && l->tuned() && params[TUNE_PARAM].getValue() > 0.5f) s += l->pitch.shift;
		s += 12.f * std::round(params[OCTAVE_PARAM].getValue());
		s += params[FINE_PARAM].getValue() / 100.f;
		return s;
	}

	void process(const ProcessArgs& args) override {
		int v = version.load();
		if (v != audioVersion) {
			engine.stop();
			audioLoaded = std::atomic_load(&uiLoaded);
			audioVersion = v;
		}
		const Loaded* l = audioLoaded.get();
		const Sample* s = l ? &l->sample : nullptr;

		lights[DETECT_LIGHT].setBrightnessSmooth(busy ? 1.f : 0.f, args.sampleTime);

		float voct = inputs[VOCT_INPUT].getVoltage();
		float semisNow = knobSemis(l) + 12.f * voct;
		bool track = params[TRACK_PARAM].getValue() > 0.5f;
		if (trigIn.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f)) {
			float k = params[LEVEL_PARAM].getValue();
			float vel = inputs[VEL_INPUT].isConnected() ? clamp(inputs[VEL_INPUT].getVoltage() / 10.f, 0.f, 1.f) : 1.f;
			engine.trigger(s, semisNow, params[DECAY_PARAM].getValue(), 2.f * k * k * vel, args.sampleRate);
			lastSemis = semisNow;
		}
		float semisPlaying = track ? semisNow : lastSemis;
		float L, R;
		engine.process(s, args.sampleRate, track ? semisNow : -1000.f, L, R);
		outputs[OUT_OUTPUT].setVoltage(5.f * 0.5f * (L + R));
		outputs[ENV_OUTPUT].setVoltage(10.f * engine.lastEnv);
		float note = (l ? l->baseNote() : 60.f) + (engine.playing() ? semisPlaying : semisNow);
		outputs[NOTE_OUTPUT].setVoltage((note - 60.f) / 12.f);
		playingNote = note;
	}

	// ---- loading (runs on the loader thread) --------------------------------
	void startJob(std::function<void()> job) {
		if (loader.joinable()) loader.join();
		busy = true;
		loader = std::thread([this, job]() {
			job();
			busy = false;
		});
	}

	static bool isAudioFile(const std::string& p) {
		std::string e = system::getExtension(p);
		for (char& c : e) c = (char) std::tolower((unsigned char) c);
		return e == ".wav" || e == ".wave" || e == ".aif" || e == ".aiff" || e == ".aifc";
	}

	void loadSample(const std::string& p) {
		PitchFocus f = focus;
		startJob([this, p, f]() {
			std::shared_ptr<Loaded> l = std::make_shared<Loaded>();
			std::string err = readSample(p, l->sample);
			if (!err.empty()) {
				WARN("Tuned Perc: could not load %s: %s", p.c_str(), err.c_str());
				std::shared_ptr<Loaded> old = std::atomic_load(&uiLoaded);
				setText(old ? old->path : p, system::getFilename(p) + ": " + err);
				return;
			}
			l->path = p;
			l->sample.name = system::getStem(p);
			l->pitch = detectPitch(l->sample, f);
			INFO("Tuned Perc: %s: %s, confidence %.2f", p.c_str(),
			     l->pitch.found ? noteName(l->pitch.note, true).c_str() : "no pitch", l->pitch.confidence);
			std::atomic_store(&uiLoaded, l);
			version++;
			setText(p, "");
		});
	}

	void redetect() {
		std::shared_ptr<Loaded> cur = std::atomic_load(&uiLoaded);
		if (!cur || busy) return;
		PitchFocus f = focus;
		startJob([this, cur, f]() {
			std::shared_ptr<Loaded> l = std::make_shared<Loaded>(*cur);
			l->pitch = detectPitch(l->sample, f);
			std::atomic_store(&uiLoaded, l);
			version++;
		});
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "path", json_string(getPath().c_str()));
		json_object_set_new(root, "focus", json_integer((int) focus));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* f = json_object_get(root, "focus");
		if (f) focus = (PitchFocus) clamp((int) json_integer_value(f), 0, 2);
		json_t* p = json_object_get(root, "path");
		if (p && json_string_value(p) && *json_string_value(p)) {
			setText(json_string_value(p), "");
			loadSample(json_string_value(p));
		}
	}
};

// ---- display ---------------------------------------------------------------
static const NVGcolor C_BG = nvgRGB(0x0B, 0x10, 0x0F);
static const NVGcolor C_INK = nvgRGB(0xED, 0xEA, 0xDF);
static const NVGcolor C_SOFT = nvgRGB(0x86, 0x91, 0x8C);
static const NVGcolor C_HOT = nvgRGB(0xE9, 0xB4, 0x4C);
static const NVGcolor C_TEAL = nvgRGB(0x7F, 0xC8, 0xB4);
static const NVGcolor C_WAVE = nvgRGB(0xAF, 0xB8, 0xB2);
static const NVGcolor C_WARN = nvgRGB(0xE8, 0x6A, 0x5A);

struct PercDisplay : Widget {
	TunedPerc* module = nullptr;

	void text(NVGcontext* vg, float x, float y, const std::string& s, NVGcolor c, int align, float size) {
		nvgFontSize(vg, size);
		nvgTextAlign(vg, align);
		nvgFillColor(vg, c);
		nvgText(vg, x, y, s.c_str(), NULL);
	}
	void rect(NVGcontext* vg, float x, float y, float w, float h, NVGcolor c) {
		nvgBeginPath(vg);
		nvgRect(vg, x, y, w, h);
		nvgFillColor(vg, c);
		nvgFill(vg);
	}
	static std::string clip(const std::string& s, size_t n) {
		return s.size() <= n ? s : s.substr(0, n - 1) + "~";
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { Widget::drawLayer(args, layer); return; }
		NVGcontext* vg = args.vg;
		float W = box.size.x, H = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, W, H, mm2px(1.2f));
		nvgFillColor(vg, C_BG);
		nvgFill(vg);
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font && font->handle >= 0) nvgFontFaceId(vg, font->handle);
		float pad = mm2px(1.6f), fs = mm2px(2.4f), fss = mm2px(1.9f);
		int L = NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, R = NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE, C = NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE;
		if (!module) {
			text(vg, W / 2, H / 2, "TUNED PERC", C_INK, C, fs * 1.3f);
			return;
		}
		std::shared_ptr<Loaded> ref = std::atomic_load(&module->uiLoaded);
		const Loaded* l = ref.get();
		std::string msg = module->getMessage();
		bool busy = module->busy;

		if (!l) {
			text(vg, pad, pad + fss, busy ? "loading..." : "no sample", busy ? C_INK : C_SOFT, L, fss);
			if (!msg.empty()) {
				float y = H * 0.42f;
				for (size_t i = 0; i < msg.size() && i < 3 * 26; i += 26, y += fss * 1.25f)
					text(vg, W / 2, y, msg.substr(i, 26), C_WARN, C, fss);
			}
			else if (!busy) {
				text(vg, W / 2, H * 0.48f, "right-click > Load sample", C_INK, C, fss);
				text(vg, W / 2, H * 0.48f + fss * 1.4f, "or drop a WAV/AIFF here", C_SOFT, C, fss);
			}
			return;
		}
		const PitchResult& p = l->pitch;
		bool tuneOn = module->params[TunedPerc::TUNE_PARAM].getValue() > 0.5f;

		// line 1: name and confidence
		text(vg, pad, pad + fss, clip(busy ? "detecting..." : l->sample.name, 18), C_SOFT, L, fss);
		if (p.found)
			text(vg, W - pad, pad + fss, string::f("sure %.0f%%", p.confidence * 100.f), p.confidence >= MIN_CONFIDENCE ? C_TEAL : C_WARN, R, fss);

		// line 2: detected -> tuned
		float y2 = pad + fss + fs * 1.9f;
		if (l->tuned()) {
			text(vg, pad, y2, noteName(p.note, true), C_INK, L, fs * 1.35f);
			if (tuneOn) {
				text(vg, W / 2 + mm2px(2.f), y2, ">", C_SOFT, C, fs * 1.2f);
				text(vg, W - pad, y2, noteName((float) p.targetC), C_HOT, R, fs * 1.35f);
				text(vg, W / 2 + mm2px(2.f), y2 + fss * 1.3f, string::f("%+.2f st", p.shift), C_SOFT, C, fss * 0.9f);
			}
			else text(vg, W - pad, y2, "as is", C_SOFT, R, fs);
		}
		else {
			text(vg, pad, y2, "no clear pitch", C_WARN, L, fs);
			text(vg, pad, y2 + fss * 1.3f, "plays as recorded", C_SOFT, L, fss * 0.9f);
		}

		// waveform, listening window, pitch line
		float wy = y2 + fss * 2.2f, wh = H - wy - mm2px(4.4f), wx = pad, ww = W - 2 * pad;
		rect(vg, wx, wy, ww, wh, nvgRGB(0x12, 0x1A, 0x18));
		const Sample& s = l->sample;
		float dur = (float) s.frames() / s.rate;
		if (dur > 0.f && p.listenEnd > p.listenStart) {
			float a = p.listenStart / dur, b = std::min(1.f, p.listenEnd / dur);
			rect(vg, wx + ww * a, wy, ww * (b - a), wh, nvgRGBA(0x7F, 0xC8, 0xB4, 0x28));
		}
		nvgBeginPath(vg);
		for (int i = 0; i < (int) ww; i++) {
			int bin = std::min(PEAK_BINS - 1, (int) ((float) i / ww * PEAK_BINS));
			float a = std::min(1.f, s.peaks[bin]) * wh * 0.46f;
			nvgRect(vg, wx + i, wy + wh / 2 - a, 1.f, std::max(0.6f, 2 * a));
		}
		nvgFillColor(vg, C_WAVE);
		nvgFill(vg);
		if (p.found && dur > 0.f) {
			// +-12 semitones around the detected note fills the height
			nvgBeginPath(vg);
			bool started = false;
			for (const PitchFrame& f : p.frames) {
				if (f.clarity < p.clearAt) { started = false; continue; }
				float x = wx + ww * std::min(1.f, f.time / dur);
				float y = wy + wh * 0.5f - clamp((f.note - p.note) / 12.f, -1.f, 1.f) * wh * 0.45f;
				if (!started) nvgMoveTo(vg, x, y);
				else nvgLineTo(vg, x, y);
				started = true;
			}
			nvgStrokeColor(vg, C_HOT);
			nvgStrokeWidth(vg, 1.2f);
			nvgStroke(vg);
		}
		double pos = module->engine.newestPos();
		if (pos >= 0 && s.frames() > 0)
			rect(vg, wx + ww * clamp((float) (pos / s.frames()), 0.f, 1.f), wy, 1.f, wh, C_INK);

		// bottom line: what plays now
		float note = module->playingNote;
		if (note > -500.f) text(vg, pad, H - pad * 0.8f, "plays " + noteName(note, true), C_INK, L, fss);
		int oct = (int) std::round(module->params[TunedPerc::OCTAVE_PARAM].getValue());
		int fine = (int) std::round(module->params[TunedPerc::FINE_PARAM].getValue());
		text(vg, W - pad, H - pad * 0.8f, string::f("oct %+d fine %+dc", oct, fine), C_SOFT, R, fss * 0.9f);
		if (!msg.empty()) text(vg, W / 2, wy + fss, clip(msg, 30), C_WARN, C, fss * 0.85f);
	}
};

// ---- panel -----------------------------------------------------------------
// Positions in mm; res/make_panel.py uses the same numbers.
struct TunedPercWidget : ModuleWidget {
	TunedPercWidget(TunedPerc* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/TunedPerc.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		PercDisplay* d = createWidget<PercDisplay>(mm2px(Vec(4.0f, 13.0f)));
		d->box.size = mm2px(Vec(52.96f, 26.0f));
		d->module = module;
		addChild(d);

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.5f, 49.5f)), module, TunedPerc::OCTAVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(45.5f, 49.5f)), module, TunedPerc::FINE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.5f, 70.5f)), module, TunedPerc::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(45.5f, 70.5f)), module, TunedPerc::LEVEL_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(11.0f, 87.0f)), module, TunedPerc::TUNE_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(30.5f, 87.0f)), module, TunedPerc::TRACK_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(51.0f, 87.0f)), module, TunedPerc::DETECT_PARAM, TunedPerc::DETECT_LIGHT));

		const float x[3] = {10.5f, 30.5f, 50.5f};
		for (int i = 0; i < 3; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x[i], 100.8f)), module, TunedPerc::TRIG_INPUT + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x[i], 117.4f)), module, TunedPerc::OUT_OUTPUT + i));
		}
	}

	bool detectWasDown = false;

	// the DETECT button is read here, on the UI thread, so the audio thread
	// never waits for the detector
	void step() override {
		ModuleWidget::step();
		TunedPerc* m = dynamic_cast<TunedPerc*>(module);
		if (!m) return;
		bool down = m->params[TunedPerc::DETECT_PARAM].getValue() > 0.5f;
		if (down && !detectWasDown) m->redetect();
		detectWasDown = down;
	}

	void onPathDrop(const PathDropEvent& e) override {
		TunedPerc* m = dynamic_cast<TunedPerc*>(module);
		if (!m || e.paths.empty()) return;
		for (const std::string& p : e.paths) {
			if (system::isFile(p) && TunedPerc::isAudioFile(p)) {
				m->loadSample(p);
				break;
			}
		}
		e.consume(this);
	}

	void appendContextMenu(Menu* menu) override {
		TunedPerc* m = dynamic_cast<TunedPerc*>(module);
		if (!m) return;
		std::string path = m->getPath();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel(path.empty() ? "No sample loaded" : "Sample: " + path));
		menu->addChild(createMenuItem("Load sample...", "", [=]() {
			std::string dir = path.empty() ? "" : system::getDirectory(path);
			osdialog_filters* filters = osdialog_filters_parse("Audio:wav,wave,aif,aiff,aifc");
			char* p = osdialog_file(OSDIALOG_OPEN, dir.empty() ? NULL : dir.c_str(), NULL, filters);
			osdialog_filters_free(filters);
			if (!p) return;
			std::string s = p;
			std::free(p);
			m->loadSample(s);
		}));
		if (!path.empty()) {
			menu->addChild(createMenuItem("Next sample in folder", "", [=]() { step(m, path, 1); }));
			menu->addChild(createMenuItem("Previous sample in folder", "", [=]() { step(m, path, -1); }));
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Listen to", {"Settled pitch (after a glide)", "The whole hit", "The attack"},
			[=]() { return (size_t) m->focus; },
			[=](size_t i) {
				m->focus = (PitchFocus) i;
				m->redetect();
			}));
	}

	static void step(TunedPerc* m, const std::string& path, int dir) {
		std::vector<std::string> files;
		for (const std::string& p : system::getEntries(system::getDirectory(path))) {
			std::string fn = system::getFilename(p);
			if (!fn.empty() && fn[0] != '.' && TunedPerc::isAudioFile(p)) files.push_back(p);
		}
		if (files.empty()) return;
		std::sort(files.begin(), files.end());
		int cur = -1;
		for (int i = 0; i < (int) files.size(); i++) if (files[i] == path) cur = i;
		int n = (int) files.size();
		m->loadSample(files[((cur + dir) % n + n) % n]);
	}
};

Model* modelTunedPerc = createModel<TunedPerc, TunedPercWidget>("TunedPerc");
