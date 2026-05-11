// Snapshot exporters for the latest spectrum / waveform frame.
// All writers return true on success; the path must already be writable
// (it comes from the OS save dialog).

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

namespace signalscope::exports {

inline bool write_csv_spectrum(const std::string& path,
                               std::span<const float> data, float sr) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fputs("frequency_hz,magnitude_dbfs\n", f);
    const float ny = sr * 0.5f;
    const auto n  = data.size();
    for (std::size_t i = 0; i < n; i++) {
        const float fz = (static_cast<float>(i) / static_cast<float>(n)) * ny;
        std::fprintf(f, "%.4f,%.4f\n", fz, data[i]);
    }
    std::fclose(f);
    return true;
}

// Explicit LE writers - WAV + RAW are little-endian on disk, and
// `fwrite(&v, ...)` would emit host order (silently wrong on a BE host).
inline void wle16(std::FILE* f, std::uint16_t v) {
    const std::uint8_t b[2] = {
        static_cast<std::uint8_t>(v & 0xFF),
        static_cast<std::uint8_t>((v >> 8) & 0xFF),
    };
    std::fwrite(b, 1, 2, f);
}
inline void wle32(std::FILE* f, std::uint32_t v) {
    const std::uint8_t b[4] = {
        static_cast<std::uint8_t>(v & 0xFF),
        static_cast<std::uint8_t>((v >> 8)  & 0xFF),
        static_cast<std::uint8_t>((v >> 16) & 0xFF),
        static_cast<std::uint8_t>((v >> 24) & 0xFF),
    };
    std::fwrite(b, 1, 4, f);
}
inline void wle_f32(std::FILE* f, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    wle32(f, bits);
}

inline bool write_raw_float32(const std::string& path,
                              std::span<const float> data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    for (float v : data) wle_f32(f, v);
    std::fclose(f);
    return true;
}

// Float32 mono WAV. fmt chunk: format=3 (IEEE float), bits=32, channels=1.
// All multi-byte fields little-endian per the WAV spec.
inline bool write_wav_float32(const std::string& path,
                              std::span<const float> data, float sr) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const std::uint32_t data_bytes =
        static_cast<std::uint32_t>(data.size() * sizeof(float));
    std::fwrite("RIFF", 1, 4, f);
    wle32(f, 36 + data_bytes);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    wle32(f, 16);                  // fmt chunk size
    wle16(f, 3);                   // format = IEEE float
    wle16(f, 1);                   // channels = 1 (mono)
    wle32(f, static_cast<std::uint32_t>(sr));
    wle32(f, static_cast<std::uint32_t>(sr) * 4); // byte rate
    wle16(f, 4);                   // block align
    wle16(f, 32);                  // bits per sample
    std::fwrite("data", 1, 4, f);
    wle32(f, data_bytes);
    for (float v : data) wle_f32(f, v);
    std::fclose(f);
    return true;
}

// Int16 big-endian AIFF, mono. The sample-rate field is an 80-bit IEEE
// extended-precision float in network byte order; for integer rates this
// reduces to (exponent, mantissa) = (16383 + floor(log2 sr), sr / 2^E).
inline bool write_aiff_int16(const std::string& path,
                             std::span<const float> data, float sr) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto wbe32 = [&](std::uint32_t v) {
        std::uint8_t b[4] = {
            static_cast<std::uint8_t>(v >> 24),
            static_cast<std::uint8_t>(v >> 16),
            static_cast<std::uint8_t>(v >> 8),
            static_cast<std::uint8_t>(v),
        };
        std::fwrite(b, 1, 4, f);
    };
    auto wbe16 = [&](std::uint16_t v) {
        std::uint8_t b[2] = {
            static_cast<std::uint8_t>(v >> 8),
            static_cast<std::uint8_t>(v),
        };
        std::fwrite(b, 1, 2, f);
    };
    const auto          n           = data.size();
    const std::uint32_t data_bytes  = static_cast<std::uint32_t>(n * 2);
    const std::uint32_t ssnd_chunk  = data_bytes + 8;
    const std::uint32_t form_size   = 46 + data_bytes;

    std::fwrite("FORM", 1, 4, f);
    wbe32(form_size);
    std::fwrite("AIFF", 1, 4, f);

    // COMM chunk
    std::fwrite("COMM", 1, 4, f);
    wbe32(18);                       // chunk size
    wbe16(1);                        // channels
    wbe32(static_cast<std::uint32_t>(n));
    wbe16(16);                       // bits/sample
    // 80-bit IEEE extended for sample rate
    const int    sri = static_cast<int>(sr);
    const int    eExp = (sri > 0) ? static_cast<int>(std::floor(std::log2(sri))) : 0;
    const double frac = (sri > 0) ? (sri / std::pow(2.0, eExp)) : 0.0;
    const std::uint32_t exp80 = 16383 + eExp;
    const std::uint32_t mant  = static_cast<std::uint32_t>(frac * 2147483648.0); // 2^31
    std::uint8_t sr_be[10] = {
        static_cast<std::uint8_t>((exp80 >> 8) & 0x7F),
        static_cast<std::uint8_t>(exp80 & 0xFF),
        static_cast<std::uint8_t>((mant >> 24) & 0xFF),
        static_cast<std::uint8_t>((mant >> 16) & 0xFF),
        static_cast<std::uint8_t>((mant >> 8)  & 0xFF),
        static_cast<std::uint8_t>(mant & 0xFF),
        0, 0, 0, 0
    };
    std::fwrite(sr_be, 1, 10, f);

    // SSND chunk
    std::fwrite("SSND", 1, 4, f);
    wbe32(ssnd_chunk);
    wbe32(0); // offset
    wbe32(0); // block size

    for (std::size_t i = 0; i < n; i++) {
        float v = data[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        const int s = static_cast<int>(v * 32767.0f);
        wbe16(static_cast<std::uint16_t>(static_cast<std::int16_t>(s)));
    }
    std::fclose(f);
    return true;
}

} // namespace signalscope::exports
