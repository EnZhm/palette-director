#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace pdtest {

// Minimal 16-bit PCM mono WAV reader/writer (no external deps).
struct WavMono {
	uint32_t sample_rate = 44100;
	std::vector<int16_t> samples;

	std::vector<float> to_float() const
	{
		std::vector<float> f;
		f.reserve(samples.size());
		for (int16_t s : samples)
			f.push_back(static_cast<float>(s) / 32768.0f);
		return f;
	}

	bool write(const std::string &path) const
	{
		std::ofstream o(path, std::ios::binary);
		if (!o)
			return false;
		const uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
		const uint32_t byte_rate = sample_rate * 2; // mono 16-bit
		const uint32_t block_align = 2;

		auto u16 = [&](uint16_t v) {
			o.put(static_cast<char>(v & 0xff));
			o.put(static_cast<char>((v >> 8) & 0xff));
		};
		auto u32 = [&](uint32_t v) {
			o.put(static_cast<char>(v & 0xff));
			o.put(static_cast<char>((v >> 8) & 0xff));
			o.put(static_cast<char>((v >> 16) & 0xff));
			o.put(static_cast<char>((v >> 24) & 0xff));
		};

		o.write("RIFF", 4);
		u32(36 + data_size);
		o.write("WAVE", 4);
		o.write("fmt ", 4);
		u32(16);
		u16(1); // PCM
		u16(1); // mono
		u32(sample_rate);
		u32(byte_rate);
		u16(block_align);
		u16(16); // bits per sample
		o.write("data", 4);
		u32(data_size);
		for (int16_t s : samples)
			u16(static_cast<uint16_t>(s));
		o.flush();
		return o.good();
	}

	static bool read(const std::string &path, WavMono &out)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;

		auto rd = [&](std::size_t n) {
			std::string b(n, '\0');
			in.read(&b[0], static_cast<std::streamsize>(n));
			return b;
		};
		auto u16 = [&]() {
			std::string b = rd(2);
			return static_cast<uint16_t>(static_cast<uint8_t>(b[0]) | (static_cast<uint8_t>(b[1]) << 8));
		};
		auto u32 = [&]() {
			std::string b = rd(4);
			return static_cast<uint32_t>(static_cast<uint8_t>(b[0])) |
			       (static_cast<uint32_t>(static_cast<uint8_t>(b[1])) << 8) |
			       (static_cast<uint32_t>(static_cast<uint8_t>(b[2])) << 16) |
			       (static_cast<uint32_t>(static_cast<uint8_t>(b[3])) << 24);
		};

		if (rd(4) != "RIFF")
			return false;
		u32(); // chunk size
		if (rd(4) != "WAVE")
			return false;

		bool have_fmt = false;
		while (in) {
			std::string id = rd(4);
			if (id.size() < 4)
				break;
			uint32_t sz = u32();
			if (id == "fmt ") {
				u16(); // format
				u16(); // channels
				out.sample_rate = u32();
				u32(); // byte rate
				u16(); // block align
				u16(); // bits per sample (assume 16)
				have_fmt = true;
				// skip any remaining fmt bytes beyond the 16 we read
				if (sz > 16) {
					std::size_t extra = static_cast<std::size_t>(sz - 16);
					while (extra-- > 0)
						in.get();
				}
			} else if (id == "data") {
				const std::size_t ns = sz / 2;
				out.samples.resize(ns);
				for (std::size_t i = 0; i < ns; ++i) {
					uint16_t v = u16();
					out.samples[i] = static_cast<int16_t>(v);
				}
				return have_fmt;
			} else {
				std::size_t skip = sz;
				while (skip-- > 0)
					in.get();
			}
		}
		return false;
	}
};

} // namespace pdtest
