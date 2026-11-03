// PercEngine.hpp
// The heart of Tuned Perc, with no VCV Rack dependencies so it can be tested
// on its own (see test/engine_test.cpp).
//
// 1. detectPitch() listens to a sample once, when it is loaded:
//    - it skips the first 15 ms after the loudest point (the click of the
//      attack has no clear pitch),
//    - then cuts the rest into short overlapping frames and runs the YIN
//      method on each: YIN finds the period at which the waveform best
//      repeats itself, and says how clean that repeat is (0 = perfect),
//    - frames with a clean repeat are kept; the answer is a loudness-weighted
//      median of them. "Settled" mode ignores the first quarter of those
//      frames, which is where an 808-style kick still glides down.
// 2. The engine plays the sample once per trigger, shifted so the detected
//    note lands on the nearest C (never more than 6 semitones away), then
//    moved by V/OCT, OCTAVE and FINE.
#pragma once
#include "SampleFile.hpp"

namespace tp {

enum PitchFocus { FOCUS_SETTLED, FOCUS_WHOLE, FOCUS_ATTACK };

struct PitchFrame {
	float time = 0.f;      // seconds from the start of the sample
	float note = 0.f;      // MIDI note number (60 = C4), fractional
	float clarity = 0.f;   // 1 = perfectly periodic
	bool used = false;     // counted in the final answer
};

struct PitchResult {
	bool found = false;
	float note = 0.f;          // MIDI note, fractional
	float confidence = 0.f;    // 0..1
	float shift = 0.f;         // semitones to the nearest C
	int targetC = 0;           // MIDI note of that C
	float listenStart = 0.f, listenEnd = 0.f;  // seconds
	std::vector<PitchFrame> frames;
};

inline float freqToNote(double f) { return (float) (69.0 + 12.0 * std::log2(f / 440.0)); }

inline std::string noteName(float note, bool cents = false) {
	static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	int n = (int) std::lround(note);
	int oct = (int) std::floor(n / 12.0) - 1;
	std::string s = names[((n % 12) + 12) % 12] + std::to_string(oct);
	if (cents) {
		int c = (int) std::lround((note - n) * 100.f);
		if (c != 0) s += (c > 0 ? "+" : "") + std::to_string(c) + "c";
	}
	return s;
}

// YIN on x[0 .. 2*maxTau). Returns the period in samples (0 if none) and
// the "aperiodicity" (lower is cleaner).
inline double yinPeriod(const float* x, int W, int minTau, int maxTau, float& aperiodicity) {
	std::vector<double> d(maxTau + 1, 0.0);
	for (int tau = 1; tau <= maxTau; tau++) {
		double s = 0.0;
		for (int j = 0; j < W; j++) {
			double v = x[j] - x[j + tau];
			s += v * v;
		}
		d[tau] = s;
	}
	// cumulative mean normalised difference
	std::vector<double> c(maxTau + 1, 1.0);
	double run = 0.0;
	for (int tau = 1; tau <= maxTau; tau++) {
		run += d[tau];
		c[tau] = run > 0.0 ? d[tau] * tau / run : 1.0;
	}
	const double threshold = 0.2;
	int best = -1;
	for (int tau = minTau; tau < maxTau; tau++) {
		if (c[tau] < threshold) {
			while (tau + 1 < maxTau && c[tau + 1] < c[tau]) tau++;
			best = tau;
			break;
		}
	}
	if (best < 0) {
		best = minTau;
		for (int tau = minTau; tau < maxTau; tau++) if (c[tau] < c[best]) best = tau;
	}
	aperiodicity = (float) c[best];
	// parabolic interpolation for a fractional period
	double t = best;
	if (best > 1 && best < maxTau) {
		double a = c[best - 1], b = c[best], e = c[best + 1];
		double den = a - 2 * b + e;
		if (std::fabs(den) > 1e-12) t = best + 0.5 * (a - e) / den;
	}
	return t;
}

inline PitchResult detectPitch(const Sample& s, PitchFocus focus = FOCUS_SETTLED) {
	PitchResult r;
	int n = s.frames();
	if (n < 64) return r;
	const double rate = s.rate;
	std::vector<float> mono(n);
	for (int i = 0; i < n; i++) mono[i] = s.R.empty() ? s.L[i] : 0.5f * (s.L[i] + s.R[i]);

	// loudest point in the first 150 ms is the attack
	int searchEnd = std::min(n, (int) (0.15 * rate));
	int peakAt = 0;
	float peak = 0.f;
	for (int i = 0; i < searchEnd; i++)
		if (std::fabs(mono[i]) > peak) { peak = std::fabs(mono[i]); peakAt = i; }
	if (peak < 1e-5f) return r;

	// 30 Hz to 4 kHz
	int maxTau = std::max(8, (int) (rate / 30.0));
	int minTau = std::max(2, (int) (rate / 4000.0));
	int W = maxTau;                       // integration window
	int frameLen = W + maxTau + 1;
	int hop = std::max(1, (int) (0.008 * rate));
	int start = std::min(n - 1, peakAt + (int) (0.015 * rate));
	int stop = std::min(n, start + (int) (0.6 * rate));
	// short samples: shrink the lowest note we look for
	if (stop - start < frameLen + 3 * hop) {
		maxTau = std::max(minTau + 4, (stop - start - 3 * hop) / 2 - 1);
		W = maxTau;
		frameLen = W + maxTau + 1;
		if (stop - start < frameLen) return r;
	}
	r.listenStart = (float) (start / rate);

	float rmsMax = 0.f;
	std::vector<float> rmsOf;
	for (int p = start; p + frameLen <= stop; p += hop) {
		double e = 0.0;
		for (int j = 0; j < frameLen; j++) e += (double) mono[p + j] * mono[p + j];
		float rms = (float) std::sqrt(e / frameLen);
		if (rms < peak * 0.01f) break;        // 40 dB below the attack: stop listening
		float ap;
		double tau = yinPeriod(&mono[p], W, minTau, maxTau, ap);
		PitchFrame f;
		f.time = (float) ((p + frameLen / 2) / rate);
		f.note = tau > 0 ? freqToNote(rate / tau) : 0.f;
		f.clarity = std::max(0.f, 1.f - ap);
		r.frames.push_back(f);
		rmsOf.push_back(rms);
		rmsMax = std::max(rmsMax, rms);
		r.listenEnd = (float) ((p + frameLen) / rate);
	}
	if (r.frames.empty()) return r;

	// keep clean frames
	std::vector<int> good;
	for (int i = 0; i < (int) r.frames.size(); i++)
		if (r.frames[i].clarity > 0.75f) good.push_back(i);
	float share = (float) good.size() / r.frames.size();
	if (good.empty() || (good.size() < 2 && r.frames.size() > 2)) {
		r.confidence = share * 0.5f;
		return r;
	}
	size_t from = 0, to = good.size();
	if (focus == FOCUS_SETTLED) from = good.size() / 4;
	if (focus == FOCUS_ATTACK) to = std::max<size_t>(1, good.size() / 3);
	std::vector<std::pair<float, float>> nw;  // note, weight
	float clar = 0.f;
	for (size_t k = from; k < to; k++) {
		int i = good[k];
		r.frames[i].used = true;
		nw.push_back({r.frames[i].note, rmsOf[i] * r.frames[i].clarity});
		clar += r.frames[i].clarity;
	}
	clar /= nw.size();
	std::sort(nw.begin(), nw.end());
	float total = 0.f;
	for (auto& p : nw) total += p.second;
	float acc = 0.f;
	r.note = nw.back().first;
	for (auto& p : nw) {
		acc += p.second;
		if (acc >= total * 0.5f) { r.note = p.first; break; }
	}
	r.found = true;
	r.confidence = std::min(1.f, share * 1.15f) * std::min(1.f, (clar - 0.75f) / 0.2f + 0.2f);
	r.confidence = std::max(0.f, std::min(1.f, r.confidence));
	r.targetC = (int) std::lround(r.note / 12.0) * 12;
	r.shift = r.targetC - r.note;
	return r;
}

// ---- playback ----------------------------------------------------------------
inline float hermite(const std::vector<float>& x, double pos) {
	int n = (int) x.size();
	int i = (int) std::floor(pos);
	float t = (float) (pos - i);
	auto at = [&](int k) { return (k < 0 || k >= n) ? 0.f : x[k]; };
	float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
	float c1 = 0.5f * (x1 - xm1);
	float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
	float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
	return ((c3 * t + c2) * t + c1) * t + x0;
}

struct PercVoice {
	bool active = false;
	double pos = 0.0;
	float semis = 0.f;       // pitch when the hit started (hold mode)
	float gain = 1.f;
	bool useEnv = false;
	float env = 1.f, envMul = 1.f;
	float fade = 1.f, fadeStep = 0.f;  // retrigger fade-out
};

struct PercEngine {
	PercVoice voices[2];
	int newest = 0;
	float lastEnv = 0.f;

	static float decaySeconds(float knob) { return 0.01f * std::pow(400.f, knob); }

	// semis: total shift for this hit. decayKnob 0..1 (>= 0.99 = whole sample).
	void trigger(const Sample* s, float semis, float decayKnob, float gain, float engineRate) {
		if (!s || s->frames() < 2) return;
		// fade the playing hit out over 3 ms
		for (PercVoice& v : voices)
			if (v.active && v.fadeStep == 0.f) v.fadeStep = 1.f / (0.003f * engineRate);
		int slot = voices[0].active && !voices[1].active ? 1 : (voices[1].active && !voices[0].active ? 0 : 1 - newest);
		PercVoice& v = voices[slot];
		v = PercVoice();
		v.active = true;
		v.semis = semis;
		v.gain = gain;
		v.useEnv = decayKnob < 0.99f;
		if (v.useEnv) v.envMul = std::exp(-6.9078f / (decaySeconds(decayKnob) * engineRate));
		newest = slot;
	}

	// trackSemis: when >= -900, the newest hit follows this pitch live (track mode).
	void process(const Sample* s, float engineRate, float trackSemis, float& outL, float& outR) {
		outL = outR = 0.f;
		lastEnv = 0.f;
		if (!s) return;
		int frames = s->frames();
		for (int k = 0; k < 2; k++) {
			PercVoice& v = voices[k];
			if (!v.active) continue;
			if (v.pos >= frames) { v.active = false; continue; }
			float a = hermite(s->L, v.pos);
			float b = s->R.empty() ? a : hermite(s->R, v.pos);
			double left = frames - 1 - v.pos;
			float semis = (k == newest && trackSemis > -900.f) ? trackSemis : v.semis;
			double inc = (double) s->rate / engineRate * std::pow(2.0, semis / 12.0);
			float tail = (float) std::min(1.0, left / std::max(1.0, 0.002 * engineRate * inc));
			float g = v.env * v.fade * tail * v.gain;
			outL += a * g;
			outR += b * g;
			if (k == newest) lastEnv = v.env * v.fade * std::min(1.f, v.gain);
			v.pos += inc;
			if (v.useEnv) {
				v.env *= v.envMul;
				if (v.env < 0.001f) v.active = false;
			}
			if (v.fadeStep > 0.f) {
				v.fade -= v.fadeStep;
				if (v.fade <= 0.f) v.active = false;
			}
		}
	}

	bool playing() const { return voices[0].active || voices[1].active; }
	double newestPos() const { return voices[newest].active ? voices[newest].pos : -1.0; }
	void stop() { voices[0].active = voices[1].active = false; }
};

} // namespace tp
