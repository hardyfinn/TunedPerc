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


// A sound made of separate resonances (freq Hz, level, decay s), plus optional
// steady noise (breath on a bottle, rattle on a can).
struct Mode { double f, a, decay; };
static Sample modes(double rate, double seconds, std::vector<Mode> m, double noiseLevel = 0.0, double glide = 0.0) {
	Sample s;
	s.rate = (int) rate;
	int n = (int) (rate * seconds);
	s.L.resize(n);
	std::vector<double> ph(m.size(), 0.0);
	for (int i = 0; i < n; i++) {
		double t = i / rate;
		double v = 0;
		for (size_t k = 0; k < m.size(); k++) {
			double f = m[k].f * (1.0 + glide * std::exp(-t / 0.03));
			ph[k] += 2 * M_PI * f / rate;
			v += m[k].a * std::sin(ph[k]) * std::exp(-t / m[k].decay);
		}
		v += 0.6 * noise() * std::exp(-t / 0.004);
		v += noiseLevel * noise() * std::exp(-t / 0.15);
		s.L[i] = (float) (0.4 * v);
	}
	s.computePeaks();
	return s;
}

static bool samePitchClass(float a, float b, float tol) {
	float d = std::fmod(std::fabs(a - b), 12.f);
	return std::min(d, 12.f - d) < tol;
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
	// 12. tom: membrane modes are not harmonics (1, 1.59, 2.14, 2.30 x f0),
	//     with a small downward bend. The old detector gave up on these.
	{
		double f = 110.0;
		Sample s = modes(44100, 0.7, {{f, 1.0, 0.25}, {f * 1.59, 0.5, 0.12}, {f * 2.14, 0.35, 0.1}, {f * 2.30, 0.3, 0.08}}, 0.0, 0.04);
		PitchResult r = detectPitch(s);
		report("A2 tom, membrane modes", r);
		CHECK(r.found && r.confidence >= MIN_CONFIDENCE && std::fabs(r.note - 45.0f) < 0.3f);
	}
	// 13. blown bottle: one resonance with breath noise all the way through
	{
		Sample s = modes(44100, 0.3, {{1174.66, 1.0, 0.12}}, 0.25);
		PitchResult r = detectPitch(s);
		report("D6 bottle with breath", r);
		CHECK(r.found && r.confidence >= MIN_CONFIDENCE && std::fabs(r.note - 86.0f) < 0.2f);
	}
	// 14. struck can: clashing metal partials, the strongest is the note
	{
		Sample s = modes(44100, 0.5, {{404.0, 1.0, 0.15}, {663.0, 0.3, 0.1}, {975.0, 0.45, 0.08}, {2103.0, 0.2, 0.05}}, 0.05);
		PitchResult r = detectPitch(s);
		report("can, 404 Hz main partial", r);
		CHECK(r.found && r.confidence >= MIN_CONFIDENCE && std::fabs(r.note - freqToNote(404.0)) < 0.2f);
	}
	// 15. drum machine tom: two resonances a fifth apart, the louder one wins
	//     (not the "virtual" note an octave and a fifth below)
	{
		Sample s = modes(44100, 0.5, {{95.0, 1.0, 0.18}, {65.0, 0.45, 0.2}});
		PitchResult r = detectPitch(s);
		report("tom 95 Hz + 65 Hz", r);
		CHECK(r.found && r.confidence >= MIN_CONFIDENCE && std::fabs(r.note - freqToNote(95.0)) < 0.25f);
	}
	// 16. bass hit with a missing fundamental: only harmonics 2, 3, 5, 6, 9 of C1
	{
		double f = 32.703;
		Sample s = modes(44100, 0.8, {{2 * f, 0.6, 0.4}, {3 * f, 1.0, 0.4}, {5 * f, 0.35, 0.3}, {6 * f, 0.7, 0.3}, {9 * f, 0.35, 0.2}});
		PitchResult r = detectPitch(s);
		report("C1 bass, no fundamental", r);
		CHECK(r.found && r.confidence >= MIN_CONFIDENCE && samePitchClass(r.note, 24.0f, 0.2f));
	}
	// 17. tabla-like: harmonics 2-6 of D#4, fundamental silent. The note
	//     name must be D# (the octave does not change the shift to C).
	{
		double f = 311.13;
		Sample s = modes(44100, 0.8, {{2 * f, 0.7, 0.3}, {3 * f, 1.0, 0.3}, {4 * f, 0.15, 0.2}, {5 * f, 0.1, 0.15}, {6 * f, 0.05, 0.1}});
		PitchResult r = detectPitch(s);
		report("tabla-like, D# harmonics", r);
		CHECK(r.found && r.confidence >= MIN_CONFIDENCE && samePitchClass(r.note, 63.0f, 0.2f));
	}
	// 18. 30 ms square blip at G6
	{
		Sample s;
		s.rate = 44100;
		for (int i = 0; i < 44100 / 4; i++) {
			double t = i / 44100.0;
			double v = std::sin(2 * M_PI * 1567.98 * t) > 0 ? 1.0 : -1.0;
			s.L.push_back((float) (0.8 * v * std::exp(-t / 0.006)));
		}
		s.computePeaks();
		PitchResult r = detectPitch(s);
		report("30 ms G6 blip", r);
		CHECK(r.found && r.confidence >= MIN_CONFIDENCE && samePitchClass(r.note, 91.0f, 0.3f));
	}
	// 19. a clap (noise bursts) and a hat with a faint hum in the tail stay untuned
	{
		Sample s;
		s.rate = 44100;
		for (int i = 0; i < 44100 / 2; i++) {
			double t = i / 44100.0;
			double env = 0;
			for (int b = 0; b < 4; b++) if (t >= b * 0.011) env += std::exp(-(t - b * 0.011) / (b == 3 ? 0.06 : 0.004));
			s.L.push_back((float) (0.4 * noise() * env + 0.003 * std::sin(2 * M_PI * 50 * t)));
		}
		s.computePeaks();
		PitchResult r = detectPitch(s);
		report("clap with 50 Hz hum", r);
		CHECK(!r.found || r.confidence < MIN_CONFIDENCE);
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
