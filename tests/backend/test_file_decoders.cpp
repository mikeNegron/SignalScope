// File-decoder tests. Synthesizes minimal WAV / AIFF / raw files in a
// per-test tmpdir, decodes them, and asserts both metadata (sr, channels,
// floats_per_frame) and sample values round-trip.
// Coverage:
//   - parse_sigmf_datatype: well-formed strings + rejection of garbage
//   - WavDecoder: int16 mono, float32 stereo
//   - AiffDecoder: int16 mono (big-endian PCM)
//   - RawDecoder: f32 mono real, cf32_le complex
//   - Failure modes: missing file, bad magic, truncated header
//   - rewind() restarts the read pointer
// Tests run in their own tmpdirs and clean up on TearDown. Synthesizing
// in C++ keeps the tests self-contained - no checked-in fixture files
// that drift out of sync with format expectations.

#include <gtest/gtest.h>

#include "audio/file_decoders.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using signalscope::file_decoder::AiffDecoder;
using signalscope::file_decoder::Datatype;
using signalscope::file_decoder::RawDecoder;
using signalscope::file_decoder::SampleType;
using signalscope::file_decoder::WavDecoder;
using signalscope::file_decoder::parse_sigmf_datatype;

namespace {

// Tmpdir fixture
class TmpDirTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    void SetUp() override {
        char tmpl[] = "/tmp/sigscope-decoder-XXXXXX";
        char *p = ::mkdtemp(tmpl);
        ASSERT_NE(p, nullptr);
        dir = p;
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::filesystem::path path(const std::string &name) const {
        return dir / name;
    }
};

// LE / BE byte writers
void write_u16_le(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
void write_u32_le(std::vector<uint8_t> &b, uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i)));
}
void write_u16_be(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
void write_u32_be(std::vector<uint8_t> &b, uint32_t v) {
    for (int i = 3; i >= 0; i--) b.push_back(uint8_t(v >> (8 * i)));
}
void write_f32_le(std::vector<uint8_t> &b, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    write_u32_le(b, bits);
}
void write_bytes(std::vector<uint8_t> &b, const char *s, std::size_t n) {
    for (std::size_t i = 0; i < n; i++) b.push_back(uint8_t(s[i]));
}

// IEEE 754 80-bit big-endian encoding for AIFF's sample-rate field.
// Restricted to positive integer rates - sufficient for tests.
std::array<uint8_t, 10> encode_f80_be_uint(uint64_t rate) {
    std::array<uint8_t, 10> out{};
    if (rate == 0) return out;
    int e = 63;
    while (e >= 0 && !((rate >> e) & 1ULL)) e--;
    uint64_t mant = rate << (63 - e);
    uint16_t expon = static_cast<uint16_t>(e + 16383);
    out[0] = uint8_t(expon >> 8);
    out[1] = uint8_t(expon);
    for (int i = 0; i < 8; i++) {
        out[2 + i] = uint8_t(mant >> (56 - 8 * i));
    }
    return out;
}

void write_file(const std::filesystem::path &p, const std::vector<uint8_t> &b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char *>(b.data()),
            static_cast<std::streamsize>(b.size()));
}

// Synthesizers
// Minimal RIFF/WAVE PCM-int16 with `samples` mono frames.
std::vector<uint8_t> make_wav_int16(uint32_t sr, uint32_t channels,
                                    const std::vector<int16_t> &samples)
{
    const uint32_t bps = 16;
    const uint32_t block_align = channels * (bps / 8);
    const uint32_t byte_rate = sr * block_align;
    const uint32_t data_size = uint32_t(samples.size() * 2);
    const uint32_t fmt_size = 16;
    const uint32_t riff_size = 4 + (8 + fmt_size) + (8 + data_size);

    std::vector<uint8_t> b;
    write_bytes(b, "RIFF", 4);
    write_u32_le(b, riff_size);
    write_bytes(b, "WAVE", 4);
    write_bytes(b, "fmt ", 4);
    write_u32_le(b, fmt_size);
    write_u16_le(b, 0x0001);  // PCM
    write_u16_le(b, uint16_t(channels));
    write_u32_le(b, sr);
    write_u32_le(b, byte_rate);
    write_u16_le(b, uint16_t(block_align));
    write_u16_le(b, uint16_t(bps));
    write_bytes(b, "data", 4);
    write_u32_le(b, data_size);
    for (int16_t v : samples) write_u16_le(b, uint16_t(v));
    return b;
}

// Minimal RIFF/WAVE float32 stereo (interleaved L, R).
std::vector<uint8_t> make_wav_float32_stereo(uint32_t sr,
                                             const std::vector<float> &lr_interleaved)
{
    const uint32_t bps = 32;
    const uint32_t channels = 2;
    const uint32_t block_align = channels * (bps / 8);
    const uint32_t byte_rate = sr * block_align;
    const uint32_t data_size = uint32_t(lr_interleaved.size() * 4);
    const uint32_t fmt_size = 16;
    const uint32_t riff_size = 4 + (8 + fmt_size) + (8 + data_size);

    std::vector<uint8_t> b;
    write_bytes(b, "RIFF", 4);
    write_u32_le(b, riff_size);
    write_bytes(b, "WAVE", 4);
    write_bytes(b, "fmt ", 4);
    write_u32_le(b, fmt_size);
    write_u16_le(b, 0x0003);  // IEEE float
    write_u16_le(b, uint16_t(channels));
    write_u32_le(b, sr);
    write_u32_le(b, byte_rate);
    write_u16_le(b, uint16_t(block_align));
    write_u16_le(b, uint16_t(bps));
    write_bytes(b, "data", 4);
    write_u32_le(b, data_size);
    for (float v : lr_interleaved) write_f32_le(b, v);
    return b;
}

// Minimal FORM/AIFF int16 mono. Big-endian sample data.
std::vector<uint8_t> make_aiff_int16(uint32_t sr, uint32_t channels,
                                     const std::vector<int16_t> &samples)
{
    const uint32_t bps = 16;
    const uint32_t comm_size = 18;
    const uint32_t data_size = uint32_t(samples.size() * 2);
    const uint32_t ssnd_size = 8 + data_size; // offset(4) + blockSize(4) + samples
    const uint32_t form_size = 4 + (8 + comm_size) + (8 + ssnd_size);

    std::vector<uint8_t> b;
    write_bytes(b, "FORM", 4);
    write_u32_be(b, form_size);
    write_bytes(b, "AIFF", 4);

    // COMM chunk
    write_bytes(b, "COMM", 4);
    write_u32_be(b, comm_size);
    write_u16_be(b, uint16_t(channels));
    write_u32_be(b, uint32_t(samples.size() / channels)); // numSampleFrames
    write_u16_be(b, uint16_t(bps));
    auto sr_bytes = encode_f80_be_uint(sr);
    for (uint8_t x : sr_bytes) b.push_back(x);

    // SSND chunk
    write_bytes(b, "SSND", 4);
    write_u32_be(b, ssnd_size);
    write_u32_be(b, 0); // offset
    write_u32_be(b, 0); // blockSize
    for (int16_t v : samples) write_u16_be(b, uint16_t(v));
    return b;
}

} // namespace

// parse_sigmf_datatype
TEST(SigmfDatatype, ComplexFloat32LittleEndian) {
    Datatype dt;
    EXPECT_TRUE(parse_sigmf_datatype("cf32_le", dt));
    EXPECT_EQ(dt.type, SampleType::F32);
    EXPECT_TRUE(dt.complex);
    EXPECT_TRUE(dt.little);
}

TEST(SigmfDatatype, RealInt16BigEndian) {
    Datatype dt;
    EXPECT_TRUE(parse_sigmf_datatype("ri16_be", dt));
    EXPECT_EQ(dt.type, SampleType::I16);
    EXPECT_FALSE(dt.complex);
    EXPECT_FALSE(dt.little);
}

TEST(SigmfDatatype, NoSuffixDefaultsToLittleEndian) {
    Datatype dt;
    // SigMF spec says LE is the default when no _le/_be suffix is given.
    EXPECT_TRUE(parse_sigmf_datatype("rf32", dt));
    EXPECT_TRUE(dt.little);
}

TEST(SigmfDatatype, RejectsGarbage) {
    Datatype dt;
    EXPECT_FALSE(parse_sigmf_datatype("",          dt));
    EXPECT_FALSE(parse_sigmf_datatype("xf32_le",   dt));  // bad complex prefix
    EXPECT_FALSE(parse_sigmf_datatype("rx32_le",   dt));  // bad type prefix
    EXPECT_FALSE(parse_sigmf_datatype("rf_le",     dt));  // missing bits
    EXPECT_FALSE(parse_sigmf_datatype("rf32_xx",   dt));  // bad endian suffix
    EXPECT_FALSE(parse_sigmf_datatype("rf48_le",   dt));  // 48-bit not supported
    EXPECT_FALSE(parse_sigmf_datatype("ri9_le",    dt));  // 9-bit not supported
}

// WAV
TEST_F(TmpDirTest, WavInt16MonoRoundTrip) {
    const std::vector<int16_t> samples = {0, 16384, -16384, 32767, -32768, 100};
    write_file(path("a.wav"), make_wav_int16(/*sr=*/48000, /*ch=*/1, samples));

    WavDecoder dec;
    ASSERT_TRUE(dec.open(path("a.wav").string()));
    EXPECT_EQ(dec.sample_rate(), 48000u);
    EXPECT_EQ(dec.channels(), 1u);
    EXPECT_FALSE(dec.is_complex());
    EXPECT_EQ(dec.floats_per_frame(), 1u);

    std::vector<float> out;
    const std::size_t got = dec.read_frames(out, samples.size());
    ASSERT_EQ(got, samples.size());
    ASSERT_EQ(out.size(), samples.size());

    // Each int16 should map to exactly v / 32768.
    for (std::size_t i = 0; i < samples.size(); i++) {
        EXPECT_FLOAT_EQ(out[i], samples[i] / 32768.0f) << "sample " << i;
    }
}

TEST_F(TmpDirTest, WavFloat32StereoCollapsesToMono) {
    // The decoder averages stereo channels into a single float (DSP path
    // is mono today). Verify the average is what comes out.
    const std::vector<float> lr = {
        1.0f, 0.0f,    //   avg 0.5
        0.5f, -0.5f,   //   avg 0.0
        -1.0f, -1.0f,  //   avg -1.0
        0.25f, 0.75f,  //   avg 0.5
    };
    write_file(path("b.wav"), make_wav_float32_stereo(44100, lr));

    WavDecoder dec;
    ASSERT_TRUE(dec.open(path("b.wav").string()));
    EXPECT_EQ(dec.sample_rate(), 44100u);
    EXPECT_EQ(dec.channels(), 2u);
    EXPECT_EQ(dec.floats_per_frame(), 1u); // collapsed to mono

    std::vector<float> out;
    const std::size_t got = dec.read_frames(out, 4);
    ASSERT_EQ(got, 4u);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_FLOAT_EQ(out[0],  0.5f);
    EXPECT_FLOAT_EQ(out[1],  0.0f);
    EXPECT_FLOAT_EQ(out[2], -1.0f);
    EXPECT_FLOAT_EQ(out[3],  0.5f);
}

TEST_F(TmpDirTest, WavRewindRestartsRead) {
    const std::vector<int16_t> samples = {1, 2, 3, 4, 5};
    write_file(path("c.wav"), make_wav_int16(48000, 1, samples));

    WavDecoder dec;
    ASSERT_TRUE(dec.open(path("c.wav").string()));

    std::vector<float> first;
    EXPECT_EQ(dec.read_frames(first, 100), samples.size());

    std::vector<float> empty;
    EXPECT_EQ(dec.read_frames(empty, 100), 0u);  // EOF

    dec.rewind();
    std::vector<float> second;
    EXPECT_EQ(dec.read_frames(second, 100), samples.size());
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); i++) {
        EXPECT_EQ(first[i], second[i]) << "sample " << i;
    }
}

TEST_F(TmpDirTest, WavRejectsBadMagic) {
    std::vector<uint8_t> b;
    write_bytes(b, "XXXX", 4);   // not RIFF
    write_u32_le(b, 0);
    write_bytes(b, "WAVE", 4);
    write_file(path("bad.wav"), b);

    WavDecoder dec;
    EXPECT_FALSE(dec.open(path("bad.wav").string()));
}

TEST_F(TmpDirTest, WavRejectsMissingFile) {
    WavDecoder dec;
    EXPECT_FALSE(dec.open((dir / "nope.wav").string()));
}

TEST_F(TmpDirTest, WavRejectsTruncatedHeader) {
    // Only the RIFF magic, no fmt/data - open() must not crash and must
    // return false. Catches future regressions where short reads might
    // throw or read past EOF.
    std::vector<uint8_t> b;
    write_bytes(b, "RIFF", 4);
    write_u32_le(b, 0);
    write_bytes(b, "WAVE", 4);
    write_file(path("trunc.wav"), b);

    WavDecoder dec;
    EXPECT_FALSE(dec.open(path("trunc.wav").string()));
}

// AIFF
TEST_F(TmpDirTest, AiffInt16MonoRoundTrip) {
    const std::vector<int16_t> samples = {0, 8192, -8192, 16384, 100, -100};
    write_file(path("a.aiff"), make_aiff_int16(48000, 1, samples));

    AiffDecoder dec;
    ASSERT_TRUE(dec.open(path("a.aiff").string()));
    EXPECT_EQ(dec.sample_rate(), 48000u);
    EXPECT_EQ(dec.channels(), 1u);
    EXPECT_FALSE(dec.is_complex());

    std::vector<float> out;
    const std::size_t got = dec.read_frames(out, samples.size());
    ASSERT_EQ(got, samples.size());
    for (std::size_t i = 0; i < samples.size(); i++) {
        EXPECT_FLOAT_EQ(out[i], samples[i] / 32768.0f) << "sample " << i;
    }
}

TEST_F(TmpDirTest, AiffSampleRate44100EncodesCorrectly) {
    // Specifically exercises the 80-bit IEEE encoder; 44100 has a
    // different mantissa pattern from 48000 so it's a useful second case.
    const std::vector<int16_t> samples = {0, 1, 2, 3};
    write_file(path("b.aiff"), make_aiff_int16(44100, 1, samples));

    AiffDecoder dec;
    ASSERT_TRUE(dec.open(path("b.aiff").string()));
    EXPECT_EQ(dec.sample_rate(), 44100u);
}

// Raw decoder
TEST_F(TmpDirTest, RawF32MonoReal) {
    const std::vector<float> samples = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f};
    std::vector<uint8_t> b;
    for (float v : samples) write_f32_le(b, v);
    write_file(path("a.f32"), b);

    Datatype dt;
    dt.type = SampleType::F32;
    dt.little = true;
    dt.complex = false;
    dt.channels = 1;
    dt.sample_rate = 48000;

    RawDecoder dec;
    ASSERT_TRUE(dec.open(path("a.f32").string()));
    dec.set_datatype(dt);
    EXPECT_EQ(dec.sample_rate(), 48000u);
    EXPECT_FALSE(dec.is_complex());
    EXPECT_EQ(dec.floats_per_frame(), 1u);

    std::vector<float> out;
    const std::size_t got = dec.read_frames(out, samples.size());
    ASSERT_EQ(got, samples.size());
    for (std::size_t i = 0; i < samples.size(); i++) {
        EXPECT_FLOAT_EQ(out[i], samples[i]) << "sample " << i;
    }
}

TEST_F(TmpDirTest, RawComplexFloat32Interleaved) {
    // SigMF cf32_le: complex, interleaved I/Q, float32, little-endian.
    const std::vector<float> iq = {
        1.0f,  0.0f,    // bin 0: re=1,  im=0
        0.0f,  1.0f,    // bin 1: re=0,  im=1
       -1.0f,  0.0f,    // bin 2: re=-1, im=0
        0.5f, -0.5f,    // bin 3: re=0.5, im=-0.5
    };
    std::vector<uint8_t> b;
    for (float v : iq) write_f32_le(b, v);
    write_file(path("a.sigmf-data"), b);

    Datatype dt;
    ASSERT_TRUE(parse_sigmf_datatype("cf32_le", dt));
    dt.sample_rate = 1000000;

    RawDecoder dec;
    ASSERT_TRUE(dec.open(path("a.sigmf-data").string()));
    dec.set_datatype(dt);
    EXPECT_TRUE(dec.is_complex());
    EXPECT_EQ(dec.floats_per_frame(), 2u);
    EXPECT_EQ(dec.sample_rate(), 1000000u);

    std::vector<float> out;
    const std::size_t frames = dec.read_frames(out, 4);
    ASSERT_EQ(frames, 4u);
    ASSERT_EQ(out.size(), 8u);
    for (std::size_t i = 0; i < 8; i++) {
        EXPECT_FLOAT_EQ(out[i], iq[i]) << "interleaved index " << i;
    }
}

TEST_F(TmpDirTest, RawI16LittleEndianScalesTo32768) {
    const std::vector<int16_t> samples = {0, 32767, -32768, 16384};
    std::vector<uint8_t> b;
    for (int16_t v : samples) write_u16_le(b, uint16_t(v));
    write_file(path("a.raw"), b);

    Datatype dt;
    dt.type = SampleType::I16;
    dt.little = true;
    dt.complex = false;
    dt.channels = 1;
    dt.sample_rate = 96000;

    RawDecoder dec;
    ASSERT_TRUE(dec.open(path("a.raw").string()));
    dec.set_datatype(dt);

    std::vector<float> out;
    const std::size_t got = dec.read_frames(out, samples.size());
    ASSERT_EQ(got, samples.size());
    EXPECT_FLOAT_EQ(out[0],  0.0f);
    EXPECT_FLOAT_EQ(out[1],  32767.0f / 32768.0f);
    EXPECT_FLOAT_EQ(out[2], -1.0f);
    EXPECT_FLOAT_EQ(out[3],  0.5f);
}

TEST_F(TmpDirTest, RawRewindRestartsRead) {
    const std::vector<float> samples = {0.1f, 0.2f, 0.3f};
    std::vector<uint8_t> b;
    for (float v : samples) write_f32_le(b, v);
    write_file(path("a.f32"), b);

    Datatype dt;
    dt.type = SampleType::F32;
    RawDecoder dec;
    ASSERT_TRUE(dec.open(path("a.f32").string()));
    dec.set_datatype(dt);

    std::vector<float> first;
    EXPECT_EQ(dec.read_frames(first, 100), samples.size());
    EXPECT_EQ(dec.read_frames(first, 100), 0u);  // EOF

    dec.rewind();
    std::vector<float> second;
    EXPECT_EQ(dec.read_frames(second, 100), samples.size());
}

TEST_F(TmpDirTest, RawShortReadStopsAtEof) {
    // Write 3 samples worth of bytes, ask for 10 frames, expect 3.
    const std::vector<float> samples = {1.0f, 2.0f, 3.0f};
    std::vector<uint8_t> b;
    for (float v : samples) write_f32_le(b, v);
    write_file(path("short.f32"), b);

    Datatype dt;
    dt.type = SampleType::F32;
    RawDecoder dec;
    ASSERT_TRUE(dec.open(path("short.f32").string()));
    dec.set_datatype(dt);

    std::vector<float> out;
    EXPECT_EQ(dec.read_frames(out, 10), samples.size());
    EXPECT_EQ(out.size(), samples.size());
}
