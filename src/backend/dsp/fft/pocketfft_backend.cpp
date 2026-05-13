#include "pocketfft_backend.hpp"

// Vendored single-header pocketfft. Defines pocketfft::r2c / c2c with a
// scale factor; we pass 1.0f to match FFTW's unnormalized forward DFT.
#include "pocketfft_hdronly.h"

#include <vector>

namespace ss::fft {

namespace {

class PocketfftBackend final : public IFftBackend {
public:
    void resize(std::size_t n) override {
        n_ = n;
        in_real_.assign(n, 0.0f);
        out_r2c_.assign(n / 2 + 1, {});
        in_complex_.assign(n, {});
        out_complex_.assign(n, {});
    }

    float*               input_real()     noexcept override { return in_real_.data(); }
    std::complex<float>* output_r2c()     noexcept override { return out_r2c_.data(); }
    std::complex<float>* input_complex()  noexcept override { return in_complex_.data(); }
    std::complex<float>* output_complex() noexcept override { return out_complex_.data(); }

    void execute_r2c() noexcept override {
        const pocketfft::shape_t  shape{n_};
        const pocketfft::stride_t stride_in {sizeof(float)};
        const pocketfft::stride_t stride_out{sizeof(std::complex<float>)};
        pocketfft::r2c(shape, stride_in, stride_out, /*axis=*/0,
                       pocketfft::FORWARD, in_real_.data(), out_r2c_.data(),
                       /*fct=*/1.0f);
    }

    void execute_c2c_forward() noexcept override {
        const pocketfft::shape_t  shape{n_};
        const pocketfft::stride_t stride{sizeof(std::complex<float>)};
        const pocketfft::shape_t  axes{0};
        pocketfft::c2c(shape, stride, stride, axes, pocketfft::FORWARD,
                       in_complex_.data(), out_complex_.data(), /*fct=*/1.0f);
    }

    std::string_view name() const noexcept override { return "pocketfft"; }

private:
    std::size_t n_ = 0;
    std::vector<float>               in_real_;
    std::vector<std::complex<float>> out_r2c_;
    std::vector<std::complex<float>> in_complex_;
    std::vector<std::complex<float>> out_complex_;
};

}  // namespace

std::unique_ptr<IFftBackend> make_pocketfft_backend() {
    return std::make_unique<PocketfftBackend>();
}

}  // namespace ss::fft
