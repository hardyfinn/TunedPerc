// SampleFile.hpp
// Reads WAV and AIFF files into float channels at the file's own sample rate.
// No VCV Rack dependencies. (Shared with Randrum.)
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace tp {

static const int PEAK_BINS = 160;

struct Sample {
	std::string name;
	std::vector<float> L, R;   // R is empty for mono files
	int rate = 48000;
	float peaks[PEAK_BINS] = {};
	int frames() const { return (int) L.size(); }

	void computePeaks() {
		int n = frames();
		for (int b = 0; b < PEAK_BINS; b++) {
			int s = (int) ((int64_t) n * b / PEAK_BINS), e = (int) ((int64_t) n * (b + 1) / PEAK_BINS);
			float m = 0.f;
			for (int i = s; i < e; i++) {
				m = std::max(m, std::fabs(L[i]));
				if (!R.empty()) m = std::max(m, std::fabs(R[i]));
			}
			peaks[b] = m;
		}
	}
};

// ---- reading WAV and AIFF files ---------------------------------------------
inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
inline uint16_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t be32(const uint8_t* p) { return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
inline uint16_t be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

// Turns interleaved sample bytes into float channels.
// enc: 0 = signed PCM (8-bit WAV is unsigned, handled by unsigned8), 1 = float.
inline std::string decodeFrames(const uint8_t* data, size_t dataLen, int channels, int bits, bool isFloat,
                                bool bigEndian, bool unsigned8, uint32_t rate, double maxSeconds, Sample& out) {
	if (channels < 1 || rate < 1000 || rate > 768000) return "bad header";
	if (isFloat ? !(bits == 32 || bits == 64) : !(bits == 8 || bits == 16 || bits == 24 || bits == 32))
		return "unsupported format (" + std::to_string(bits) + "-bit " + (isFloat ? "float" : "PCM") + ")";
	int bps = bits / 8;
	size_t frames = dataLen / ((size_t) bps * channels);
	frames = std::min(frames, (size_t) (maxSeconds * rate));
	if (frames < 2) return "no audio in the file";
	auto sample = [&](size_t fr, int ch) -> float {
		const uint8_t* s = data + (fr * channels + ch) * bps;
		uint8_t b[8];
		for (int i = 0; i < bps; i++) b[i] = bigEndian ? s[bps - 1 - i] : s[i];  // now little-endian
		if (isFloat) {
			if (bits == 32) { float v; std::memcpy(&v, b, 4); return v; }
			double v; std::memcpy(&v, b, 8); return (float) v;
		}
		switch (bits) {
			case 8: return unsigned8 ? (b[0] - 128) / 128.f : (int8_t) b[0] / 128.f;
			case 16: return (int16_t) rd16(b) / 32768.f;
			case 24: { int32_t v = (int32_t) (((uint32_t) b[0] << 8) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 24)); return v / 2147483648.f; }
			default: return (int32_t) rd32(b) / 2147483648.f;
		}
	};
	out.rate = (int) rate;
	out.L.resize(frames);
	out.R.clear();
	if (channels > 1) out.R.resize(frames);
	for (size_t i = 0; i < frames; i++) {
		out.L[i] = sample(i, 0);
		if (channels > 1) out.R[i] = sample(i, 1);
	}
	out.computePeaks();
	return "";
}

inline std::string parseWav(const std::vector<uint8_t>& d, Sample& out, double maxSeconds) {
	int fmt = 0, channels = 0, bits = 0;
	uint32_t rate = 0;
	const uint8_t* data = nullptr;
	size_t dataLen = 0;
	size_t p = 12;
	while (p + 8 <= d.size()) {
		uint32_t len = rd32(&d[p + 4]);
		const uint8_t* body = &d[p + 8];
		size_t avail = std::min<size_t>(len, d.size() - p - 8);
		if (!std::memcmp(&d[p], "fmt ", 4) && avail >= 16) {
			fmt = rd16(body);
			channels = rd16(body + 2);
			rate = rd32(body + 4);
			bits = rd16(body + 14);
			if (fmt == 0xFFFE && avail >= 26) fmt = rd16(body + 24);
		}
		else if (!std::memcmp(&d[p], "data", 4)) {
			data = body;
			// some writers leave the size as 0 or 0xFFFFFFFF: use the rest of the file
			dataLen = (len == 0 || len == 0xFFFFFFFFu) ? d.size() - p - 8 : avail;
			if (len == 0 || len == 0xFFFFFFFFu) break;
		}
		p += 8 + (size_t) len + (len & 1);
	}
	if (!data) return "no audio data";
	if (fmt != 1 && fmt != 3) return "compressed WAV (format " + std::to_string(fmt) + ")";
	return decodeFrames(data, dataLen, channels, bits, fmt == 3, false, true, rate, maxSeconds, out);
}

inline double extended80(const uint8_t* b) {
	int e = ((b[0] & 0x7F) << 8) | b[1];
	uint64_t m = 0;
	for (int i = 0; i < 8; i++) m = (m << 8) | b[2 + i];
	if (e == 0 && m == 0) return 0.0;
	double v = std::ldexp((double) m, e - 16383 - 63);
	return (b[0] & 0x80) ? -v : v;
}

inline std::string parseAiff(const std::vector<uint8_t>& d, Sample& out, double maxSeconds) {
	bool aifc = !std::memcmp(&d[8], "AIFC", 4);
	int channels = 0, bits = 0;
	uint32_t frames = 0;
	double rate = 0;
	bool isFloat = false, little = false;
	const uint8_t* data = nullptr;
	size_t dataLen = 0;
	size_t p = 12;
	while (p + 8 <= d.size()) {
		uint32_t len = be32(&d[p + 4]);
		const uint8_t* body = &d[p + 8];
		size_t avail = std::min<size_t>(len, d.size() - p - 8);
		if (!std::memcmp(&d[p], "COMM", 4) && avail >= 18) {
			channels = be16(body);
			frames = be32(body + 2);
			bits = be16(body + 6);
			rate = extended80(body + 8);
			if (aifc && avail >= 22) {
				const uint8_t* c = body + 18;
				if (!std::memcmp(c, "sowt", 4)) little = true;
				else if (!std::memcmp(c, "fl32", 4) || !std::memcmp(c, "FL32", 4)) { isFloat = true; bits = 32; }
				else if (!std::memcmp(c, "fl64", 4) || !std::memcmp(c, "FL64", 4)) { isFloat = true; bits = 64; }
				else if (std::memcmp(c, "NONE", 4) && std::memcmp(c, "twos", 4))
					return "compressed AIFF (" + std::string((const char*) c, 4) + ")";
			}
		}
		else if (!std::memcmp(&d[p], "SSND", 4) && avail >= 8) {
			uint32_t off = be32(body);
			if (8 + (size_t) off <= avail) {
				data = body + 8 + off;
				dataLen = avail - 8 - off;
			}
		}
		p += 8 + (size_t) len + (len & 1);
	}
	if (!data) return "no audio data";
	if (!isFloat) bits = (bits + 7) / 8 * 8;  // e.g. 12-bit is stored in 16
	if (channels > 0 && bits > 0) dataLen = std::min(dataLen, (size_t) frames * channels * (bits / 8));
	return decodeFrames(data, dataLen, channels, bits, isFloat, !little, false, (uint32_t) std::lround(rate), maxSeconds, out);
}

// Reads a WAV or AIFF file (8/16/24/32-bit PCM or 32/64-bit float, any
// sample rate, mono or stereo; extra channels are ignored), keeping the file's
// own sample rate. Returns "" on success, or the reason it failed.
inline std::string readSample(const std::string& path, Sample& out, double maxSeconds = 20.0) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return "could not open (macOS may be blocking access to this folder)";
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size < 12) { std::fclose(f); return "file too small"; }
	if (size > 512L * 1024 * 1024) { std::fclose(f); return "file too big"; }
	std::vector<uint8_t> d(size);
	size_t got = std::fread(d.data(), 1, size, f);
	std::fclose(f);
	if (got != (size_t) size) return "could not read";
	if (!std::memcmp(&d[0], "RIFF", 4) && !std::memcmp(&d[8], "WAVE", 4)) return parseWav(d, out, maxSeconds);
	if (!std::memcmp(&d[0], "FORM", 4) && (!std::memcmp(&d[8], "AIFF", 4) || !std::memcmp(&d[8], "AIFC", 4))) return parseAiff(d, out, maxSeconds);
	return "not a WAV or AIFF file";
}

} // namespace tp
