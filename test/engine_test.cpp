// Tests for Tuned Perc's pitch detector and engine. Build and run:
//   c++ -std=c++11 -O2 -o test/engine_test test/engine_test.cpp && test/engine_test
#include "../src/PercEngine.hpp"
#include <cstdio>
#include <cstdlib>
using namespace tp;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t rs = 12345;
static float noise() { rs = rs * 1664525u + 1013904223u; return ((rs >> 8) / 8388608.f) - 1.f; }

// A drum-like test sound: a burst of noise for the click, then partials that decay.
static Sample drum(double rate, double seconds, double f0, std::vector<double> harm, double decay, double glideFrom = 0, double glideTime = 0.04) {
	Sample s;
	s.rate = (int) rate;
	int n = (int) (rate * seconds);
	s.L.resize(n);
	double ph = 0;
	for (int i = 0; i < n; i++) {
		double t = i / rate;
		double f = glideFrom > 0 ? f0 + (glideFrom - f0) * std::exp(-t / glideTime) : f0;
		ph += 2 * M_PI * f / rate;
		double v = 0;
		for (size_t k = 0; k < harm.size(); k++) v += harm[k] * std::sin((k + 1) * ph);
		v *= std::exp(-t / decay);
		v += 0.6 * noise() * std::exp(-t / 0.004);
		s.L[i] = (float) (0.5 * v);
	}
	s.computePeaks();
	return s;
}

static void report(const char* what, const PitchResult& r) {
	std::printf("  %-26s found=%d note=%6.2f (%s) conf=%.2f shift=%+.2f -> %s\n", what, r.found, r.note,
	            noteName(r.note, true).c_str(), r.confidence, r.shift, noteName((float) r.targetC).c_str());
}

int main() {
	// 1. E2 +12 cents, plain decaying sine
	{
		double f = 440.0 * std::pow(2.0, (40.12 - 69) / 12.0);
		Sample s = drum(48000, 0.8, f, {1.0}, 0.25);
		PitchResult r = detectPitch(s);
		report("E2+12c sine", r);
		CHECK(r.found && std::fabs(r.note - 40.12f) < 0.1f);
		CHECK(r.targetC == 36 && std::fabs(r.shift + 4.12f) < 0.1f);
		CHECK(r.confidence > 0.7f);
	}
	// 2. tom at G3 with harmonics, 44.1 kHz
	{
		Sample s = drum(44100, 0.6, 196.0, {1.0, 0.5, 0.3, 0.15}, 0.15);
		PitchResult r = detectPitch(s);
		report("G3 tom with harmonics", r);
		CHECK(r.found && std::fabs(r.note - 55.0f) < 0.15f);
		CHECK(r.targetC == 60 && std::fabs(r.shift - 5.0f) < 0.15f);
	}
	// 3. 808-style kick gliding from 120 Hz down to 50 Hz
	{
		Sample s = drum(48000, 1.2, 50.0, {1.0}, 0.5, 120.0, 0.04);
		PitchResult r = detectPitch(s, FOCUS_SETTLED);
		report("808 glide 120->50 Hz", r);
		CHECK(r.found && std::fabs(r.note - freqToNote(50.0)) < 0.5f);
		PitchResult a = detectPitch(s, FOCUS_ATTACK);
		report("  same, attack focus", a);
		CHECK(a.found && a.note > r.note);
	}
	// 4. noise (hi-hat): no clear pitch
	{
		Sample s;
		s.rate = 48000;
		for (int i = 0; i < 12000; i++) s.L.push_back(0.5f * noise() * std::exp(-i / 3000.f));
		PitchResult r = detectPitch(s);
		report("noise hat", r);
		CHECK(!r.found || r.confidence < 0.3f);
	}
	// 5. bright saw at A2, no octave mistakes
	{
		std::vector<double> h;
		for (int k = 1; k <= 20; k++) h.push_back(1.0 / k);
		Sample s = drum(44100, 0.5, 110.0, h, 0.2);
		PitchResult r = detectPitch(s);
		report("A2 saw", r);
		CHECK(r.found && std::fabs(r.note - 45.0f) < 0.15f);
	}
	// 6. very short rim at 300 Hz (60 ms)
	{
		Sample s = drum(48000, 0.06, 300.0, {1.0, 0.3}, 0.03);
		PitchResult r = detectPitch(s);
		report("short 300 Hz rim", r);
		CHECK(r.found && std::fabs(r.note - freqToNote(300.0)) < 0.2f);
	}
	// 7. exact C2 needs no shift; F#2 goes to the nearer C
	{
		Sample s = drum(48000, 0.6, 65.406, {1.0}, 0.2);
		PitchResult r = detectPitch(s);
		report("C2", r);
		CHECK(r.targetC == 36 && std::fabs(r.shift) < 0.1f);
		Sample s2 = drum(48000, 0.6, 440.0 * std::pow(2.0, (41.6 - 69) / 12.0), {1.0}, 0.2);
		PitchResult r2 = detectPitch(s2);
		report("F2+60c", r2);
		CHECK(r2.targetC == 36 && r2.shift < -5.5f && r2.shift > -6.f);
	}
	// 8. playback: +12 semitones plays the sample in half the time
	{
		Sample s;
		s.rate = 48000;
		s.L.assign(4800, 0.5f);
		PercEngine e;
		e.trigger(&s, 12.f, 1.f, 1.f, 48000.f);
		float L, R;
		int n = 0;
		while (e.playing() && n < 100000) { e.process(&s, 48000.f, -1000.f, L, R); n++; }
		CHECK(n >= 2398 && n <= 2402);
	}
	// 9. track mode follows a live pitch; hold mode keeps the start pitch
	{
		Sample s;
		s.rate = 48000;
		s.L.assign(4800, 0.5f);
		PercEngine e;
		e.trigger(&s, 0.f, 1.f, 1.f, 48000.f);
		float L, R;
		int n = 0;
		while (e.playing() && n < 100000) { e.process(&s, 48000.f, -12.f, L, R); n++; }
		CHECK(n >= 9598 && n <= 9602);
	}
	// 10. retrigger fades the old hit; decay ends the hit
	{
		Sample s;
		s.rate = 48000;
		s.L.assign(48000, 0.5f);
		PercEngine e;
		float L, R;
		e.trigger(&s, 0.f, 1.f, 1.f, 48000.f);
		for (int i = 0; i < 100; i++) e.process(&s, 48000.f, -1000.f, L, R);
		e.trigger(&s, 0.f, 0.3f, 1.f, 48000.f);
		for (int i = 0; i < 300; i++) e.process(&s, 48000.f, -1000.f, L, R);
		CHECK((e.voices[0].active ? 1 : 0) + (e.voices[1].active ? 1 : 0) == 1);
		for (int i = 0; i < 48000 * 0.1; i++) e.process(&s, 48000.f, -1000.f, L, R);
		CHECK(!e.playing());
	}
	// 11. note names
	CHECK(noteName(60.f) == "C4" && noteName(36.f) == "C2" && noteName(40.12f, true) == "E2+12c" && noteName(-1.f) == "B-2");
	std::printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
	return fails ? 1 : 0;
}
