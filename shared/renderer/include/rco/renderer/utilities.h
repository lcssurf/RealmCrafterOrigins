#pragma once
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <ratio>

namespace rco::renderer {

namespace detail {
    inline thread_local uint64_t xshf_x = 123456789;
    inline thread_local uint64_t xshf_y = 362436069;
    inline thread_local uint64_t xshf_z = 521288629;

    inline uint64_t xorshf96() {
        xshf_x ^= xshf_x << 16;
        xshf_x ^= xshf_x >> 5;
        xshf_x ^= xshf_x << 1;
        uint64_t t = xshf_x;
        xshf_x = xshf_y;
        xshf_y = xshf_z;
        xshf_z = t ^ xshf_x ^ xshf_y;
        return xshf_z;
    }
}

class Timer {
public:
    Timer() : beg_(clock_::now()) {}
    void reset() { beg_ = clock_::now(); }
    double elapsed() const {
        return std::chrono::duration_cast<second_>(clock_::now() - beg_).count();
    }
private:
    using clock_  = std::chrono::high_resolution_clock;
    using second_ = std::chrono::duration<double, std::ratio<1>>;
    std::chrono::time_point<clock_> beg_;
};

inline double rng() {
    uint64_t bits = 1023ull << 52ull | (detail::xorshf96() & 0xfffffffffffffull);
    return *reinterpret_cast<double*>(&bits) - 1.0;
}

template<typename T, typename Q>
T map(T val, Q r1s, Q r1e, Q r2s, Q r2e) {
    return (val - r1s) / (r1e - r1s) * (r2e - r2s) + r2s;
}

inline double rng(double low, double high) {
    return map(rng(), 0.0, 1.0, low, high);
}

inline void hash_combine(std::size_t&) {}

template <typename T, typename... Rest>
void hash_combine(std::size_t& seed, const T& v, Rest... rest) {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    hash_combine(seed, rest...);
}

} // namespace rco::renderer
