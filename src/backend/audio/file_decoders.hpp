#pragma once
// File decoders for the file-source feed. All decoders convert their native
// sample format to interleaved float32 and report sample-rate / channel-count
// / complex-or-real to the caller.
// Supported: WAV (PCM int16/24/32 + IEEE float32/64), AIFF (PCM int16/24/32 +
// IEEE float), and a generic raw reader driven by an explicit datatype
// descriptor (used for `.raw`, `.f32`, and `.sigmf-data`).
// All readers stream - they don't load the whole file into memory.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace signalscope::file_decoder {

// Datatype descriptor for raw / .f32 / .sigmf-data — formats that
// aren't self-describing on disk.

enum class SampleType : uint8_t {
    F32, F64,
    I32, I16, I8,
    U8,
};

struct Datatype {
    SampleType type      = SampleType::F32;
    bool       little    = true;   // little-endian on disk
    bool       complex   = false;  // I/Q interleaved
    uint32_t   channels  = 1;      // 1 = mono, 2 = stereo, ... (independent of complex)
    uint32_t   sample_rate = 48000;

    size_t bytes_per_sample() const {
        switch (type) {
            case SampleType::F32: return 4;
            case SampleType::F64: return 8;
            case SampleType::I32: return 4;
            case SampleType::I16: return 2;
            case SampleType::I8:  return 1;
            case SampleType::U8:  return 1;
        }
        return 0;
    }

    // Number of float32 *outputs* produced by one disk frame.
    // - real mono:    1
    // - real stereo:  1 (we collapse to mono by averaging - DSP is mono today)
    // - complex mono: 2 (I, Q interleaved)
    size_t floats_per_frame() const { return complex ? 2 : 1; }

    // Number of *disk* sample slots per logical frame.
    // - real mono:    1
    // - real stereo:  channels
    // - complex mono: 2 (I + Q)
    size_t disk_slots_per_frame() const {
        return complex ? 2 : channels;
    }
};

// SigMF datatype string parser. e.g. "cf32_le", "ri16_le", "rf64_be".
inline bool parse_sigmf_datatype(const std::string& s, Datatype& out) {
    // Format: <r|c><f|i|u><bits>[_<le|be>]
    // Minimum form is 3 chars (e.g. "ri8" - 1-byte types need no endian
    // suffix). Endian defaults to little when omitted.
    if (s.size() < 3) return false;
    char rc = s[0]; char fi = s[1];
    if (rc != 'r' && rc != 'c') return false;
    if (fi != 'f' && fi != 'i' && fi != 'u') return false;
    out.complex = (rc == 'c');

    size_t bits_start = 2;
    size_t bits_end = bits_start;
    while (bits_end < s.size() && std::isdigit(static_cast<unsigned char>(s[bits_end]))) bits_end++;
    if (bits_end == bits_start) return false;
    int bits = std::stoi(s.substr(bits_start, bits_end - bits_start));

    out.little = true; // default per SigMF
    if (bits_end < s.size() && s[bits_end] == '_') {
        std::string suffix = s.substr(bits_end + 1);
        if (suffix == "be") out.little = false;
        else if (suffix == "le") out.little = true;
        else return false;
    }

    if (fi == 'f') {
        if (bits == 32) out.type = SampleType::F32;
        else if (bits == 64) out.type = SampleType::F64;
        else return false;
    } else if (fi == 'i') {
        if (bits == 32) out.type = SampleType::I32;
        else if (bits == 16) out.type = SampleType::I16;
        else if (bits == 8)  out.type = SampleType::I8;
        else return false;
    } else /* u */ {
        if (bits == 8) out.type = SampleType::U8;
        else return false;
    }
    return true;
}

// Endian-aware readers
inline uint16_t read_u16_le(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t read_u32_le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint16_t read_u16_be(const uint8_t* p) { return (uint16_t(p[0]) << 8) | uint16_t(p[1]); }
inline uint32_t read_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// IEEE 754 80-bit extended precision (used by AIFF for sample rate).
inline double read_f80_be(const uint8_t* p) {
    uint16_t expon = (uint16_t(p[0]) << 8) | p[1];
    uint64_t hi = (uint64_t(p[2]) << 24) | (uint64_t(p[3]) << 16) | (uint64_t(p[4]) << 8) | p[5];
    uint64_t lo = (uint64_t(p[6]) << 24) | (uint64_t(p[7]) << 16) | (uint64_t(p[8]) << 8) | p[9];
    uint64_t mant = (hi << 32) | lo;
    if (expon == 0 && mant == 0) return 0.0;
    int sign = (expon & 0x8000) ? -1 : 1;
    int e = (expon & 0x7FFF) - 16383;
    double f = static_cast<double>(mant) * std::ldexp(1.0, e - 63);
    return sign * f;
}

// Sample -> float32 conversion
inline float i16_to_f32(int16_t v) { return static_cast<float>(v) / 32768.0f; }
inline float i24_to_f32(int32_t v) { return static_cast<float>(v) / 8388608.0f; }
inline float i32_to_f32(int32_t v) { return static_cast<float>(v) / 2147483648.0f; }
inline float i8_to_f32(int8_t  v) { return static_cast<float>(v) / 128.0f; }
inline float u8_to_f32(uint8_t v) { return (static_cast<float>(v) - 128.0f) / 128.0f; }

// Read int24 LE. Sign-extends 24-bit two's complement to int32.
inline int32_t read_i24_le(const uint8_t* p) {
    int32_t v = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
    if (v & 0x800000) v |= 0xFF000000u;
    return v;
}
inline int32_t read_i24_be(const uint8_t* p) {
    int32_t v = (int32_t(p[0]) << 16) | (int32_t(p[1]) << 8) | int32_t(p[2]);
    if (v & 0x800000) v |= 0xFF000000u;
    return v;
}

class IDecoder {
public:
    virtual ~IDecoder() = default;

    // Open the file. Returns true on success.
    virtual bool open(const std::string& path) = 0;

    // Sample rate of the file in Hz.
    virtual uint32_t sample_rate() const = 0;

    // True if file carries complex (IQ) samples.
    virtual bool is_complex() const = 0;

    // Number of channels (mono / stereo / multichannel).
    virtual uint32_t channels() const = 0;

    // Read up to `frames` worth of samples, append to `out` as interleaved
    // float32 in the layout described by `floats_per_frame()`. Returns the
    // number of frames actually decoded; 0 means EOF.
    virtual size_t read_frames(std::vector<float>& out, size_t frames) = 0;

    // Rewind to the start (for looping).
    virtual void rewind() = 0;

    // floats_per_frame: 1 for real-mono, 1 for real-stereo (collapsed), 2 for complex.
    virtual size_t floats_per_frame() const = 0;
};

// WAV decoder (RIFF/WAVE)
// Supports PCM int16, int24, int32, and IEEE float32 / float64.
// Both standard and WAVE_FORMAT_EXTENSIBLE (subformat GUID) headers.

class WavDecoder : public IDecoder {
public:
    ~WavDecoder() override { close(); }

    bool open(const std::string& path) override {
        close();
        fp_ = std::fopen(path.c_str(), "rb");
        if (!fp_) return false;
        if (!parse_header()) { close(); return false; }
        data_start_ = std::ftell(fp_);
        return true;
    }

    uint32_t sample_rate() const override { return sample_rate_; }
    bool is_complex() const override { return false; }
    uint32_t channels() const override { return channels_; }
    size_t floats_per_frame() const override { return 1; }

    void rewind() override {
        if (fp_ && data_start_ >= 0) std::fseek(fp_, data_start_, SEEK_SET);
        bytes_remaining_ = data_size_;
    }

    size_t read_frames(std::vector<float>& out, size_t frames) override {
        if (!fp_ || frames == 0) return 0;
        const size_t bps = bits_per_sample_ / 8;
        const size_t frame_bytes = bps * channels_;
        const size_t want_bytes = frames * frame_bytes;
        const size_t avail_bytes = std::min(bytes_remaining_, want_bytes);
        if (avail_bytes == 0) return 0;
        scratch_.resize(avail_bytes);
        size_t got = std::fread(scratch_.data(), 1, avail_bytes, fp_);
        if (got == 0) return 0;
        bytes_remaining_ -= got;
        const size_t got_frames = got / frame_bytes;

        const size_t out_start = out.size();
        out.resize(out_start + got_frames);
        for (size_t i = 0; i < got_frames; i++) {
            // Average channels into mono float (DSP path is mono today).
            float sum = 0.0f;
            for (uint32_t c = 0; c < channels_; c++) {
                const uint8_t* p = scratch_.data() + (i * channels_ + c) * bps;
                sum += sample_to_float(p);
            }
            out[out_start + i] = sum / static_cast<float>(channels_);
        }
        return got_frames;
    }

private:
    void close() {
        if (fp_) { std::fclose(fp_); fp_ = nullptr; }
    }

    bool parse_header() {
        uint8_t hdr[12];
        if (std::fread(hdr, 1, 12, fp_) != 12) return false;
        if (std::memcmp(hdr, "RIFF", 4) != 0)  return false;
        if (std::memcmp(hdr + 8, "WAVE", 4) != 0) return false;

        bool got_fmt = false;
        while (true) {
            uint8_t chunk[8];
            if (std::fread(chunk, 1, 8, fp_) != 8) return false;
            uint32_t cksz = read_u32_le(chunk + 4);
            if (std::memcmp(chunk, "fmt ", 4) == 0) {
                std::vector<uint8_t> fmt(cksz);
                if (std::fread(fmt.data(), 1, cksz, fp_) != cksz) return false;
                uint16_t fmt_tag = read_u16_le(fmt.data());
                channels_         = read_u16_le(fmt.data() + 2);
                sample_rate_      = read_u32_le(fmt.data() + 4);
                bits_per_sample_  = read_u16_le(fmt.data() + 14);

                // 0x0001 = PCM, 0x0003 = IEEE float, 0xFFFE = EXTENSIBLE
                if (fmt_tag == 0xFFFE && cksz >= 40) {
                    // SubFormat GUID first 2 bytes encode the actual format tag.
                    fmt_tag = read_u16_le(fmt.data() + 24);
                }
                if (fmt_tag != 0x0001 && fmt_tag != 0x0003) return false;
                is_float_ = (fmt_tag == 0x0003);
                got_fmt = true;
                if ((cksz & 1) && std::fseek(fp_, 1, SEEK_CUR) != 0) return false;
            } else if (std::memcmp(chunk, "data", 4) == 0) {
                if (!got_fmt) return false;
                data_size_ = cksz;
                bytes_remaining_ = cksz;
                return true;
            } else {
                // skip unknown chunk
                if (std::fseek(fp_, cksz + (cksz & 1), SEEK_CUR) != 0) return false;
            }
        }
    }

    float sample_to_float(const uint8_t* p) const {
        if (is_float_) {
            if (bits_per_sample_ == 32) {
                float f; std::memcpy(&f, p, 4); return f;
            } else if (bits_per_sample_ == 64) {
                double d; std::memcpy(&d, p, 8); return static_cast<float>(d);
            }
        } else {
            if (bits_per_sample_ == 16) {
                int16_t v = static_cast<int16_t>(read_u16_le(p));
                return i16_to_f32(v);
            } else if (bits_per_sample_ == 24) {
                return i24_to_f32(read_i24_le(p));
            } else if (bits_per_sample_ == 32) {
                int32_t v = static_cast<int32_t>(read_u32_le(p));
                return i32_to_f32(v);
            }
        }
        return 0.0f;
    }

    std::FILE*  fp_ = nullptr;
    long        data_start_ = -1;
    size_t      bytes_remaining_ = 0;
    size_t      data_size_ = 0;
    uint32_t    sample_rate_ = 0;
    uint32_t    channels_ = 1;
    uint16_t    bits_per_sample_ = 16;
    bool        is_float_ = false;
    std::vector<uint8_t> scratch_;
};

// AIFF decoder (IFF/AIFF, big-endian)
// Supports PCM int16/24/32 (FORM/AIFF) and IEEE float (FORM/AIFC, fl32/fl64).

class AiffDecoder : public IDecoder {
public:
    ~AiffDecoder() override { close(); }

    bool open(const std::string& path) override {
        close();
        fp_ = std::fopen(path.c_str(), "rb");
        if (!fp_) return false;
        if (!parse_header()) { close(); return false; }
        data_start_ = std::ftell(fp_);
        return true;
    }

    uint32_t sample_rate() const override { return sample_rate_; }
    bool is_complex() const override { return false; }
    uint32_t channels() const override { return channels_; }
    size_t floats_per_frame() const override { return 1; }

    void rewind() override {
        if (fp_ && data_start_ >= 0) std::fseek(fp_, data_start_, SEEK_SET);
        bytes_remaining_ = data_size_;
    }

    size_t read_frames(std::vector<float>& out, size_t frames) override {
        if (!fp_ || frames == 0) return 0;
        const size_t bps = bits_per_sample_ / 8;
        const size_t frame_bytes = bps * channels_;
        const size_t want_bytes = frames * frame_bytes;
        const size_t avail_bytes = std::min(bytes_remaining_, want_bytes);
        if (avail_bytes == 0) return 0;
        scratch_.resize(avail_bytes);
        size_t got = std::fread(scratch_.data(), 1, avail_bytes, fp_);
        if (got == 0) return 0;
        bytes_remaining_ -= got;
        const size_t got_frames = got / frame_bytes;
        const size_t out_start = out.size();
        out.resize(out_start + got_frames);
        for (size_t i = 0; i < got_frames; i++) {
            float sum = 0.0f;
            for (uint32_t c = 0; c < channels_; c++) {
                const uint8_t* p = scratch_.data() + (i * channels_ + c) * bps;
                sum += sample_to_float(p);
            }
            out[out_start + i] = sum / static_cast<float>(channels_);
        }
        return got_frames;
    }

private:
    void close() {
        if (fp_) { std::fclose(fp_); fp_ = nullptr; }
    }

    bool parse_header() {
        uint8_t hdr[12];
        if (std::fread(hdr, 1, 12, fp_) != 12) return false;
        if (std::memcmp(hdr, "FORM", 4) != 0) return false;
        bool aifc = (std::memcmp(hdr + 8, "AIFC", 4) == 0);
        bool aiff = (std::memcmp(hdr + 8, "AIFF", 4) == 0);
        if (!aifc && !aiff) return false;

        bool got_comm = false;
        while (true) {
            uint8_t chunk[8];
            if (std::fread(chunk, 1, 8, fp_) != 8) return false;
            uint32_t cksz = read_u32_be(chunk + 4);
            if (std::memcmp(chunk, "COMM", 4) == 0) {
                std::vector<uint8_t> comm(cksz);
                if (std::fread(comm.data(), 1, cksz, fp_) != cksz) return false;
                channels_         = read_u16_be(comm.data());
                // numSampleFrames at offset 2 (uint32) - informational
                bits_per_sample_  = read_u16_be(comm.data() + 6);
                double sr         = read_f80_be(comm.data() + 8);
                sample_rate_      = static_cast<uint32_t>(sr);
                is_float_ = false;
                if (aifc && cksz >= 22) {
                    // Compression type FOURCC at offset 18.
                    char ct[4]; std::memcpy(ct, comm.data() + 18, 4);
                    if (std::memcmp(ct, "fl32", 4) == 0 ||
                        std::memcmp(ct, "FL32", 4) == 0) { is_float_ = true; bits_per_sample_ = 32; }
                    else if (std::memcmp(ct, "fl64", 4) == 0 ||
                             std::memcmp(ct, "FL64", 4) == 0) { is_float_ = true; bits_per_sample_ = 64; }
                    else if (std::memcmp(ct, "NONE", 4) == 0 ||
                             std::memcmp(ct, "none", 4) == 0 ||
                             std::memcmp(ct, "twos", 4) == 0) { /* PCM */ }
                    else return false; // unsupported codec
                }
                got_comm = true;
                if (cksz & 1) std::fseek(fp_, 1, SEEK_CUR);
            } else if (std::memcmp(chunk, "SSND", 4) == 0) {
                if (!got_comm) return false;
                if (cksz < 8) return false;
                uint8_t ssnd_hdr[8];
                if (std::fread(ssnd_hdr, 1, 8, fp_) != 8) return false;
                uint32_t offset = read_u32_be(ssnd_hdr);
                if (offset > 0 && std::fseek(fp_, offset, SEEK_CUR) != 0) return false;
                data_size_ = cksz - 8 - offset;
                bytes_remaining_ = data_size_;
                return true;
            } else {
                if (std::fseek(fp_, cksz + (cksz & 1), SEEK_CUR) != 0) return false;
            }
        }
    }

    float sample_to_float(const uint8_t* p) const {
        if (is_float_) {
            if (bits_per_sample_ == 32) {
                uint32_t bits = read_u32_be(p);
                float f; std::memcpy(&f, &bits, 4); return f;
            } else if (bits_per_sample_ == 64) {
                uint64_t hi = read_u32_be(p);
                uint64_t lo = read_u32_be(p + 4);
                uint64_t bits = (hi << 32) | lo;
                double d; std::memcpy(&d, &bits, 8); return static_cast<float>(d);
            }
        } else {
            if (bits_per_sample_ == 16) {
                int16_t v = static_cast<int16_t>(read_u16_be(p));
                return i16_to_f32(v);
            } else if (bits_per_sample_ == 24) {
                return i24_to_f32(read_i24_be(p));
            } else if (bits_per_sample_ == 32) {
                int32_t v = static_cast<int32_t>(read_u32_be(p));
                return i32_to_f32(v);
            }
        }
        return 0.0f;
    }

    std::FILE*  fp_ = nullptr;
    long        data_start_ = -1;
    size_t      bytes_remaining_ = 0;
    size_t      data_size_ = 0;
    uint32_t    sample_rate_ = 0;
    uint32_t    channels_ = 1;
    uint16_t    bits_per_sample_ = 16;
    bool        is_float_ = false;
    std::vector<uint8_t> scratch_;
};

// Raw decoder (driven by a Datatype descriptor)
// Used for `.raw`, `.f32`, and `.sigmf-data`. The format is fully described
// by the caller-supplied Datatype.

class RawDecoder : public IDecoder {
public:
    ~RawDecoder() override { close(); }

    bool open(const std::string& path) override {
        close();
        fp_ = std::fopen(path.c_str(), "rb");
        if (!fp_) return false;
        std::fseek(fp_, 0, SEEK_END);
        long sz = std::ftell(fp_);
        if (sz < 0) { close(); return false; }
        std::fseek(fp_, 0, SEEK_SET);
        file_size_ = static_cast<size_t>(sz);
        bytes_remaining_ = file_size_;
        return true;
    }

    void set_datatype(const Datatype& dt) { dt_ = dt; }

    uint32_t sample_rate() const override { return dt_.sample_rate; }
    bool is_complex() const override { return dt_.complex; }
    uint32_t channels() const override { return dt_.channels; }
    size_t floats_per_frame() const override { return dt_.floats_per_frame(); }

    void rewind() override {
        if (fp_) std::fseek(fp_, 0, SEEK_SET);
        bytes_remaining_ = file_size_;
    }

    size_t read_frames(std::vector<float>& out, size_t frames) override {
        if (!fp_ || frames == 0) return 0;
        const size_t bps = dt_.bytes_per_sample();
        const size_t slots = dt_.disk_slots_per_frame();
        const size_t frame_bytes = bps * slots;
        if (frame_bytes == 0) return 0;
        const size_t want_bytes = frames * frame_bytes;
        const size_t avail_bytes = std::min(bytes_remaining_, want_bytes);
        if (avail_bytes == 0) return 0;
        scratch_.resize(avail_bytes);
        size_t got = std::fread(scratch_.data(), 1, avail_bytes, fp_);
        if (got == 0) return 0;
        bytes_remaining_ -= got;
        const size_t got_frames = got / frame_bytes;

        const size_t out_per_frame = dt_.floats_per_frame();
        const size_t out_start = out.size();
        out.resize(out_start + got_frames * out_per_frame);

        for (size_t i = 0; i < got_frames; i++) {
            const uint8_t* base = scratch_.data() + i * frame_bytes;
            if (dt_.complex) {
                // Interleaved I, Q
                out[out_start + i * 2 + 0] = sample_to_float(base + 0 * bps);
                out[out_start + i * 2 + 1] = sample_to_float(base + 1 * bps);
            } else if (dt_.channels == 1) {
                out[out_start + i] = sample_to_float(base);
            } else {
                float sum = 0.0f;
                for (uint32_t c = 0; c < dt_.channels; c++) sum += sample_to_float(base + c * bps);
                out[out_start + i] = sum / static_cast<float>(dt_.channels);
            }
        }
        return got_frames;
    }

private:
    void close() {
        if (fp_) { std::fclose(fp_); fp_ = nullptr; }
    }

    float sample_to_float(const uint8_t* p) const {
        switch (dt_.type) {
            case SampleType::F32: {
                uint32_t bits = dt_.little ? read_u32_le(p) : read_u32_be(p);
                float f; std::memcpy(&f, &bits, 4); return f;
            }
            case SampleType::F64: {
                uint64_t hi = dt_.little ? read_u32_le(p + 4) : read_u32_be(p);
                uint64_t lo = dt_.little ? read_u32_le(p + 0) : read_u32_be(p + 4);
                uint64_t bits = (hi << 32) | lo;
                double d; std::memcpy(&d, &bits, 8); return static_cast<float>(d);
            }
            case SampleType::I32: {
                uint32_t bits = dt_.little ? read_u32_le(p) : read_u32_be(p);
                return i32_to_f32(static_cast<int32_t>(bits));
            }
            case SampleType::I16: {
                uint16_t bits = dt_.little ? read_u16_le(p) : read_u16_be(p);
                return i16_to_f32(static_cast<int16_t>(bits));
            }
            case SampleType::I8:
                return i8_to_f32(static_cast<int8_t>(p[0]));
            case SampleType::U8:
                return u8_to_f32(p[0]);
        }
        return 0.0f;
    }

    std::FILE*  fp_ = nullptr;
    Datatype    dt_;
    size_t      file_size_ = 0;
    size_t      bytes_remaining_ = 0;
    std::vector<uint8_t> scratch_;
};

} // namespace signalscope::file_decoder
