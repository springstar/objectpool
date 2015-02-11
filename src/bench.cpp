#include "memory_pool.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <iostream>

// Helper function: extract a value representing the `pct` percentile of a sorted sample-set, using
// linear interpolation. If samples are not sorted, return nonsensical value.
template <typename T>
T percentile_of_sorted(const std::vector<T> & sorted_samples, T pct)
{
    assert(!sorted_samples.empty());
    if (sorted_samples.size() == 1)
    {
        return sorted_samples[0];
    }
    assert(T(0) <= pct);
    const T hundred(100);
    assert(pct <= hundred);
    if (pct == hundred)
    {
        return sorted_samples[sorted_samples.size() - 1];
    }
    const auto length = (sorted_samples.size() - 1);
    const auto rank = (pct / hundred) * length;
    const auto lrank = std::floor(rank);
    const auto d = rank - lrank;
    const auto n = static_cast<size_t>(lrank);
    const auto lo = sorted_samples[n];
    const auto hi = sorted_samples[n+1];
    return lo + (hi - lo) * d;
}

// Winsorize a set of samples, replacing values above the `100-pct` percentile and below the `pct`
// percentile with those percentiles themselves. This is a way of minimizing the effect of
// outliers, at the cost of biasing the sample. It differs from trimming in that it does not
// change the number of samples, just changes the values of those that are outliers.
//
// See: http://en.wikipedia.org/wiki/Winsorising
template <typename T>
void winsorize(std::vector<T> & samples, T pct) {
    std::sort(samples.begin(), samples.end());
    auto lo = percentile_of_sorted(samples, pct);
    auto hi = percentile_of_sorted(samples, 100 - pct);
    for (auto & samp : samples)
    {
        if (samp > hi)
        {
            samp = hi;
        }
        else if (samp < lo)
        {
            samp = lo;
        }
    }
}

template <typename T>
class Summary
{
public:
    T max;
    T min;
    T median;
    T median_abs_dev;
    T median_abs_dev_pct;

    Summary() : max(0), min(0), median(0), median_abs_dev(0),
        median_abs_dev_pct(0) {}

    // TODO: This could be made a lot more efficient by reusing results of
    // previous calculations.
    Summary(const std::vector<T> & samples) :
        max(calc_max(samples)),
        min(calc_min(samples)),
        median(calc_median(samples)),
        median_abs_dev(calc_median_abs_dev(samples)),
        median_abs_dev_pct(T(100) * median_abs_dev / median) {}

private:
    T calc_max(const std::vector<T> & samples) const
    {
        return *std::max_element(samples.begin(), samples.end());
    }

    T calc_min(const std::vector<T> & samples) const
    {
        return *std::min_element(samples.begin(), samples.end());
    }

    T calc_percentile(const std::vector<T> & samples, T pct) const
    {
        std::vector<T> tmp(samples);
        std::sort(tmp.begin(), tmp.end());
        return percentile_of_sorted(tmp, pct);
    }

    T calc_median(const std::vector<T> & samples) const
    {
        return calc_percentile(samples, T(50));
    }

    T calc_median_abs_dev(const std::vector<T> & samples) const
    {
        const auto med = calc_median(samples);
        std::vector<T> abs_devs(samples);
        for (auto & v : abs_devs)
        {
            v = med - v;
        }
        return calc_median(abs_devs) * T(1.4826);
    }
};

class Bencher
{
    uint64_t iterations_ = 0;
    std::chrono::nanoseconds duration_ = std::chrono::nanoseconds::zero();
    uint64_t bytes_ = 0;
public:
    uint64_t bytes() const { return bytes_; }
    std::chrono::nanoseconds ns_per_iter() const
    {
        if (iterations_ != 0)
        {
            return duration_ / iterations_;
        }
        else
        {
            return std::chrono::nanoseconds::zero();
        }
    }
    template <typename F>
    void bench_n(uint64_t iterations, F func)
    {
        iterations_ = iterations;
        duration_ = std::chrono::nanoseconds::zero();
        auto loop_start = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < iterations; ++i)
        {
            func(*this);
        }
        duration_ = std::chrono::high_resolution_clock::now() - loop_start;
    }
    template <typename F>
    Summary<double> auto_bench(F func)
    {
        // initial bench run to get ballpark figure.
        uint64_t n = 1;
        bench_n(n, func);

        // try to estimate iteration count for 1ms falling back to 1 million
        // iterations if first run too < 1ns.
        if (ns_per_iter() == std::chrono::nanoseconds::zero())
        {
            n = 1000000;
        }
        else
        {
            n = 1000000 / std::max<uint64_t>(ns_per_iter().count(), 1);
        }

        // if the first run took more than 1ms we don't want to just
        // be left doing 0 iterations on every loop. The unfortunate
        // side effect of not being able to do as many runs is
        // automatically handled by the statistical analysis below
        // (i.e. larger error bars).
        if (n == 0) { n = 1; }

        std::chrono::nanoseconds total_run;
        std::vector<double> samples(50);

        Summary<double> summ;
        Summary<double> summ5;
        for (;;)
        {
			auto loop_start = std::chrono::high_resolution_clock::now();
            for (auto & p : samples)
            {
                bench_n(n, func);
                p = ns_per_iter().count();
            }

            winsorize(samples, 5.0);
            summ = Summary<double>(samples);

            for (auto & p : samples)
            {
                bench_n(n * 5, func);
                p = ns_per_iter().count();
            }

            winsorize(samples, 5.0);
            summ5 = Summary<double>(samples);

            // If we've run for 100ms and seem to have converged to a
            // stable median.
            auto loop_run = std::chrono::high_resolution_clock::now() - loop_start;
            if (loop_run > std::chrono::milliseconds(100) &&
                summ.median_abs_dev_pct < 1.0 &&
                summ.median - summ5.median < summ5.median_abs_dev)
            {
                return summ5;
            }

            total_run = total_run + loop_run;
            // Longest we ever run for is 3s.
            if (total_run > std::chrono::seconds(3)) {
                return summ5;
            }

            n *= 2;
        }
    }
};

struct BenchSamples
{
    Summary<double> ns_iter_summ;
    uint64_t mb_s = 0;
};

std::string fmt_bench_samples(const BenchSamples & bs)
{
    char buffer[1024] = {0};
    if (bs.mb_s)
    {
        snprintf(buffer, sizeof(buffer),
            "%9" PRId64 " ns/iter (+/- %" PRId64 ") = %" PRIu64 " MB/s\n",
            static_cast<int64_t>(bs.ns_iter_summ.median),
            static_cast<int64_t>(bs.ns_iter_summ.max - bs.ns_iter_summ.min),
            bs.mb_s);
    }
    else
    {
        snprintf(buffer, sizeof(buffer),
            "%9" PRId64 " ns/iter (+/- %" PRId64 ")\n",
            static_cast<int64_t>(bs.ns_iter_summ.median),
            static_cast<int64_t>(bs.ns_iter_summ.max - bs.ns_iter_summ.min));
    }
    return buffer;
}

template <typename F>
BenchSamples benchmark(F f)
{
    BenchSamples bs;
    Bencher b;

    bs.ns_iter_summ = b.auto_bench(f);

    auto ns_iter = std::max<uint64_t>(bs.ns_iter_summ.median, 1);
    auto iter_s = 1000000000 / ns_iter;
    bs.mb_s = (b.bytes() * iter_s) / 1000000;

    return bs;
}

uint64_t factorial(uint64_t n)
{
    return n > 0 ? factorial(n - 1) * n : 1;
}

void SimpleBench(Bencher &)
{
    factorial(100);
}

int main()
{
    BenchSamples bs = benchmark(SimpleBench);
    std::cout << fmt_bench_samples(bs) << std::endl;
    return 0;
}