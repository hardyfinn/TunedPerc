// PercEngine.hpp
// The heart of Tuned Perc, with no VCV Rack dependencies so it can be tested
// on its own (see test/engine_test.cpp).
//
// 1. detectPitch() listens to a sample once, when it is loaded:
//    - it skips the click of the attack (a few ms after the loudest point),
//    - then looks at the spectrum of short overlapping frames and finds the
//      resonances: sharp peaks standing well above the noise around them.
//      Drums ring at resonances that are often not whole-number multiples of
//      each other (toms, bottles, cans, tabla), so this works where a
//      "does the waveform repeat?" test gives up,
//    - each frame's note is the strongest resonance, or the fundamental of
//      a harmonic series when the peaks form one (bass hits, synth blips),
//    - frames that are mostly resonance are kept; the answer is a
//      loudness-weighted median of them. "Settled" mode ignores the first
//      quarter of those frames, which is where an 808-style kick still
//      glides down,
//    - if that finds nothing, the older waveform-repeat (YIN) detector gets a
//      second go, for very short blips.
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
	float clearAt = 0.75f;     // frames with clarity above this were pitched
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

// ---- pitch detection ---------------------------------------------------------
// Drums rarely repeat their waveform exactly: a tom, bottle or tabla rings at
// a few resonances that are not whole-number multiples of each other, and the
// sound decays and bends while it rings. So instead of looking for a repeating
// waveform, the detector looks at the spectrum and listens for resonances:
// sharp peaks that stand well above the noise around them.

// In-place radix-2 FFT (n must be a power of two).
inline void fft(std::vector<double>& re, std::vector<double>& im) {
	int n = (int) re.size();
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
	}
	for (int len = 2; len <= n; len <<= 1) {
		double a = -2.0 * M_PI / len;
		double wr = std::cos(a), wi = std::sin(a);
		for (int i = 0; i < n; i += len) {
			double cr = 1.0, ci = 0.0;
			for (int k = 0; k < len / 2; k++) {
				int p = i + k, q = p + len / 2;
				double tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
				re[q] = re[p] - tr; im[q] = im[p] - ti;
				re[p] += tr; im[p] += ti;
				double t = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = t;
			}
		}
	}
}

struct SpectralPeak {
	double freq = 0.0;   // Hz
	double amp = 0.0;    // linear magnitude
};

// How much of the peaks' strength sits on the harmonic series of f0
// (f0, 2 f0, 3 f0...), and how many peaks that is.
// "roots" counts the harmonics that are not a multiple of another one found
// (2, 3 and 5 are three roots; 2, 4 and 6 are one).
inline double seriesShare(const std::vector<SpectralPeak>& peaks, double f0, int& roots) {
	double on = 0.0, all = 0.0;
	bool hit[17] = {};
	for (const SpectralPeak& p : peaks) {
		all += p.amp;
		double ratio = p.freq / f0;
		int k = (int) std::lround(ratio);
		if (k >= 1 && k <= 16 && std::fabs(ratio / k - 1.0) < 0.025) { on += p.amp; hit[k] = true; }
	}
	roots = 0;
	for (int k = 1; k <= 16; k++) {
		if (!hit[k]) continue;
		bool multiple = false;
		for (int j = 1; j < k && !multiple; j++) if (hit[j] && k % j == 0) multiple = true;
		if (!multiple) roots++;
	}
	return all > 0.0 ? on / all : 0.0;
}

// The frame's pitch: which resonance the ear takes as "the note".
// 1. Every strong peak is a candidate. A candidate earns its own strength
//    plus a share of each peak sitting on one of its harmonics (2x, 3x...),
//    so a bright saw or a tom with overtones lands on its fundamental, while
//    a quiet rumble far below the main tone cannot steal it.
// 2. A bass or synth hit can have a weak or missing fundamental (only 2x, 3x,
//    5x... are strong). If a lower note explains clearly more of the peaks,
//    including at least three unrelated harmonics (like 2x, 3x and 5x), that
//    note wins.
inline double pickFundamental(const std::vector<SpectralPeak>& peaks) {
	double maxAmp = 0.0;
	for (const SpectralPeak& p : peaks) maxAmp = std::max(maxAmp, p.amp);
	double best = 0.0, bestScore = 0.0;
	for (const SpectralPeak& c : peaks) {
		if (c.amp < 0.2 * maxAmp) continue;
		double score = 0.0;
		for (const SpectralPeak& p : peaks) {
			double ratio = p.freq / c.freq;
			int k = (int) std::lround(ratio);
			if (k < 1 || k > 12) continue;
			if (std::fabs(ratio / k - 1.0) > 0.03) continue;
			score += p.amp / k;
		}
		if (score > bestScore) { bestScore = score; best = c.freq; }
	}
	if (best <= 0.0) return 0.0;
	std::vector<SpectralPeak> strong;
	for (const SpectralPeak& p : peaks) if (p.amp >= 0.1 * maxAmp) strong.push_back(p);
	int cnt;
	double own = seriesShare(strong, best, cnt);
	for (int k = 2; k <= 8; k++) {
		double v = best / k;
		if (v < 30.0) break;
		double sh = seriesShare(strong, v, cnt);
		if (cnt >= 3 && sh > own + 0.15 && sh > 0.7) return v;
	}
	// 3. A strong peak a fifth below the winner (2x and 3x of a silent
	//    fundamental, as on a tabla) names the note better: it is that
	//    fundamental's octave, so the shift to C comes out right.
	double bestAmp = 0.0;
	for (const SpectralPeak& p : peaks) if (p.freq == best) bestAmp = p.amp;
	for (const SpectralPeak& p : peaks)
		if (std::fabs(p.freq * 1.5 / best - 1.0) < 0.015 && p.amp >= 0.5 * bestAmp) return p.freq;
	return best;
}

// Above this a frame counts as clearly pitched (also used by the screen).
static const float CLEAR_FRAME = 0.4f;

inline PitchResult detectPitchSpectral(const Sample& s, PitchFocus focus) {
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

	// How long does it ring? (until 40 dB below the attack)
	int ring = 0;
	{
		int blk = std::max(1, (int) (0.005 * rate));
		for (int b = peakAt; b < n; b += blk) {
			float m = 0.f;
			for (int j = b; j < std::min(n, b + blk); j++) m = std::max(m, std::fabs(mono[j]));
			if (m > peak * 0.01f) ring = std::min(n, b + blk) - peakAt;
		}
	}
	// Listen from just after the click of the attack, for up to 1.2 s.
	// The analysis frame is up to 190 ms long (enough cycles to place a 30 Hz
	// kick to a few cents), but shorter for sounds that only ring briefly.
	int start = std::min(n - 1, peakAt + std::min((int) (0.008 * rate), ring / 10));
	int stop = std::min(n, start + (int) (1.2 * rate));
	int want = std::min((int) (0.19 * rate), std::max(256, (int) (ring * 0.6)));
	int N = 256;
	while (N * 2 <= want) N *= 2;
	while (N > 256 && N > stop - start) N /= 2;
	if (stop - start < N) {
		start = std::max(0, stop - N);
		if (stop - start < N) return r;
	}
	const int PAD = 4;                      // zero padding: finer frequency grid
	int M = N * PAD;
	double binHz = rate / M;
	int lo = std::max(2, (int) (30.0 / binHz));
	int hi = std::min(M / 2 - 2, (int) (8000.0 / binHz));
	if (hi <= lo + 8) return r;
	int hop = std::max(1, (int) (0.01 * rate));
	r.listenStart = (float) (start / rate);

	std::vector<double> win(N);
	for (int j = 0; j < N; j++) win[j] = 0.5 - 0.5 * std::cos(2.0 * M_PI * (j + 0.5) / N);
	std::vector<double> re(M), im(M), mag(M / 2), lmag(M / 2), floorDb(M / 2);
	std::vector<float> rmsOf;

	for (int p = start; p + N <= stop; p += hop) {
		double mean = 0.0, e = 0.0;
		for (int j = 0; j < N; j++) mean += mono[p + j];
		mean /= N;
		for (int j = 0; j < N; j++) e += (mono[p + j] - mean) * (mono[p + j] - mean);
		float rms = (float) std::sqrt(e / N);
		if (rms < peak * 0.003f) break;        // 50 dB below the attack: stop listening
		std::fill(re.begin(), re.end(), 0.0);
		std::fill(im.begin(), im.end(), 0.0);
		for (int j = 0; j < N; j++) re[j] = (mono[p + j] - mean) * win[j];
		fft(re, im);
		for (int k = 0; k < M / 2; k++) {
			mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
			lmag[k] = 20.0 * std::log10(mag[k] + 1e-12);
		}
		// the "noise floor": median level within a third of an octave or
		// more around each bin (a resonance stands far above it, noise does not)
		{
			int step = PAD;                     // the floor varies slowly
			std::vector<double> tmp;
			for (int k = lo; k <= hi; k += step) {
				int half = std::max(8 * PAD, (int) (k * 0.12));
				int a = std::max(1, k - half), b = std::min(M / 2 - 1, k + half);
				tmp.clear();
				for (int q = a; q <= b; q += PAD) tmp.push_back(lmag[q]);
				std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
				double m = tmp[tmp.size() / 2];
				for (int q = k; q < std::min(hi + 1, k + step); q++) floorDb[q] = m;
			}
		}
		// resonances: local maxima at least 18 dB above that floor
		std::vector<SpectralPeak> peaks;
		double tonal = 0.0, total = 0.0;
		for (int k = lo; k <= hi; k++) total += mag[k] * mag[k];
		int lobe = 2 * PAD;                     // main lobe half-width of the window
		for (int k = lo; k <= hi; k++) {
			if (lmag[k] - floorDb[k] < 18.0) continue;
			bool isMax = true;
			for (int q = std::max(1, k - lobe); q <= std::min(M / 2 - 2, k + lobe) && isMax; q++)
				if (q != k && (mag[q] > mag[k] || (mag[q] == mag[k] && q < k))) isMax = false;
			if (!isMax) continue;
			// fine frequency: fit a parabola through the log magnitudes
			double a = lmag[k - 1], b = lmag[k], c = lmag[k + 1];
			double den = a - 2.0 * b + c;
			double d = std::fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
			SpectralPeak sp;
			sp.freq = (k + d) * binHz;
			sp.amp = mag[k];
			peaks.push_back(sp);
			for (int q = std::max(lo, k - lobe); q <= std::min(hi, k + lobe); q++) tonal += mag[q] * mag[q];
		}
		PitchFrame f;
		f.time = (float) ((p + N / 2) / rate);
		double f0 = peaks.empty() ? 0.0 : pickFundamental(peaks);
		// clarity: the share of the frame's energy that sits in resonances
		double share = total > 0.0 ? std::min(1.0, tonal / total) : 0.0;
		f.note = f0 > 0.0 ? freqToNote(f0) : 0.f;
		f.clarity = f0 > 0.0 ? (float) share : 0.f;
		r.frames.push_back(f);
		rmsOf.push_back(rms);
		r.listenEnd = (float) ((p + N) / rate);
	}
	r.clearAt = CLEAR_FRAME;
	if (r.frames.empty()) return r;

	// keep clearly pitched frames
	std::vector<int> good;
	for (int i = 0; i < (int) r.frames.size(); i++)
		if (r.frames[i].clarity > CLEAR_FRAME) good.push_back(i);
	// share of the sound (weighted by loudness) that is clearly pitched, so a
	// faint hum left in the tail of a clap does not count as a note
	float allW = 0.f, goodW = 0.f;
	for (int i = 0; i < (int) r.frames.size(); i++) allW += rmsOf[i];
	for (int i : good) goodW += rmsOf[i];
	float share = allW > 0.f ? goodW / allW : 0.f;
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
	// how well the frames agree with that note (within half a semitone)
	float agree = 0.f;
	for (auto& p : nw) if (std::fabs(p.first - r.note) < 0.5f) agree += p.second;
	agree = total > 0.f ? agree / total : 0.f;
	r.found = true;
	r.confidence = std::min(1.f, share * 1.3f) * std::min(1.f, (clar - CLEAR_FRAME) / 0.3f + 0.5f) * (0.4f + 0.6f * agree);
	r.confidence = std::max(0.f, std::min(1.f, r.confidence));
	r.targetC = (int) std::lround(r.note / 12.0) * 12;
	r.shift = r.targetC - r.note;
	return r;
}

// ---- second opinion: the waveform-repeat (YIN) detector from v2.1 ----------
// Kept for very short, harmonic blips (a 30 ms synth beep) where a spectrum
// has too few cycles to work with.
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

inline PitchResult detectPitchYin(const Sample& s, PitchFocus focus) {
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

// ---- the detector ----------------------------------------------------------
// Below this confidence the module plays the sample as recorded.
static const float MIN_CONFIDENCE = 0.4f;

inline PitchResult detectPitch(const Sample& s, PitchFocus focus = FOCUS_SETTLED) {
	PitchResult r = detectPitchSpectral(s, focus);
	if (r.found && r.confidence >= MIN_CONFIDENCE) return r;
	PitchResult y = detectPitchYin(s, focus);
	if (y.found && y.confidence >= MIN_CONFIDENCE && y.confidence > r.confidence) return y;
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
