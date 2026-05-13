#include "kfr_backend.hpp"

#include <kfr/dft.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace ss::fft {

namespace {

// KFR's complex<T> is layout-compatible with std::complex<T>; cast through
// when handing buffers to plan.execute().
inline kfr::complex<float>* as_kfr(std::complex<float>* p) noexcept {
    return reinterpret_cast<kfr::complex<float>*>(p);
}

class KfrBackend final : public IFftBackend {
public:
    void resize(std::size_t n) override {
        n_ = n;
        plan_r2c_ = std::make_unique<kfr::dft_plan_real<float>>(n);
        plan_c2c_ = std::make_unique<kfr::dft_plan<float>>(n);
        temp_r2c_.assign(plan_r2c_->temp_size, 0);
        temp_c2c_.assign(plan_c2c_->temp_size, 0);
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
        plan_r2c_->execute(as_kfr(out_r2c_.data()), in_real_.data(),
                           temp_r2c_.data());
    }

    void execute_c2c_forward() noexcept override {
        plan_c2c_->execute(as_kfr(out_complex_.data()),
                           as_kfr(in_complex_.data()),
                           temp_c2c_.data(),
                           /*inverse=*/false);
    }

    std::string_view name() const noexcept override { return "kfr"; }

private:
    std::size_t n_ = 0;
    std::unique_ptr<kfr::dft_plan_real<float>> plan_r2c_;
    std::unique_ptr<kfr::dft_plan<float>>      plan_c2c_;
    std::vector<std::uint8_t>          temp_r2c_;
    std::vector<std::uint8_t>          temp_c2c_;
    std::vector<float>                 in_real_;
    std::vector<std::complex<float>>   out_r2c_;
    std::vector<std::complex<float>>   in_complex_;
    std::vector<std::complex<float>>   out_complex_;
};

}  // namespace

std::unique_ptr<IFftBackend> make_kfr_backend() {
    return std::make_unique<KfrBackend>();
}

}  // namespace ss::fft
