#include "bench.hpp"

#include <mimalloc.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numeric>
#include <random>
#include <stdexcept>

namespace cb {
namespace {
using Clock = std::chrono::steady_clock;
static_assert(Clock::is_steady);
// Written only after timing. Volatile node reads plus opaque streaming calls
// retain the work; these sinks also expose its final result to the compiler.
volatile std::uint64_t checksum_sink = 0;
Node* volatile pointer_sink = nullptr;

template<class Execute, class Validate, class Convert>
Measurement sample(const Config& c, std::uint64_t quantum, std::uint64_t cap,
                   Execute execute, Validate validate, Convert convert) {
    const double target = c.sample_ms * 1e6;
    const auto max_count = cap / quantum * quantum;
    const auto grow = [max_count](std::uint64_t n) {
        return n > max_count / 8 ? max_count : n * 8;
    };
    auto count = quantum;
    const auto timed = [&](std::uint64_t& n) {
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            const auto start = Clock::now();
            execute(n);
            const auto finish = Clock::now();
            const double ns = std::chrono::duration<double, std::nano>(finish - start).count();
            validate(n); // full result checks and sink stores are OUTSIDE timing
            if (!std::isfinite(ns) || ns < 0)
                throw std::runtime_error("Clock returned an invalid elapsed time");
            if (ns > 0) return ns;
            // Equal timestamps are valid for work shorter than one timer tick.
            // Retry a larger full-cycle/pass batch; never divide by zero or
            // invent a duration. Update n so rates and counts use the final batch.
            if (n == max_count) break;
            n = grow(n);
        }
        throw std::runtime_error("Clock did not advance after increasing batch size to the retry or operation limit");
    };
    const auto adjust = [&](std::uint64_t n, double ns) {
        // Round to full cycles/passes and cap all conversion/multiplication.
        const double desired = std::ceil(static_cast<double>(n) * target * 1.1 / ns / quantum);
        const auto max_units = cap / quantum;
        const auto units = static_cast<std::uint64_t>(std::clamp(desired, 1.0, static_cast<double>(max_units)));
        return units * quantum;
    };
    execute(quantum); // warm full cycle / pass before calibration
    validate(quantum);
    bool calibrated = false;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const double ns = timed(count);
        if (ns >= target || count == max_count) {
            calibrated = true;
            break;
        }
        // Limit growth to 8x so a very short first clock reading cannot cause
        // an excessive batch. Counts are always exact full cycles/passes.
        count = std::min(adjust(count, ns), grow(count));
    }
    if (!calibrated) throw std::runtime_error("Could not calibrate within bounded attempts");

    Measurement result;
    result.min_operations = std::numeric_limits<std::uint64_t>::max();
    std::vector<double> samples;
    samples.reserve(c.repetitions);
    for (unsigned repetition = 0; repetition < c.repetitions; ++repetition) {
        execute(quantum); // full warm-up before EVERY independent timed sample
        validate(quantum);
        const double ns = timed(count);
        samples.push_back(convert(count, ns));
        result.min_operations = std::min(result.min_operations, count);
        result.max_operations = std::max(result.max_operations, count);
        count = adjust(count, ns);
    }
    result.value = aggregate(std::move(samples));
    return result;
}

constexpr std::uint64_t pattern(std::size_t index) {
    // Distinct words make a partial or broken copy observable.
    return 0x9e3779b97f4a7c15ULL * (static_cast<std::uint64_t>(index) + 1);
}
}

Buffer::Buffer(std::size_t bytes, std::size_t alignment)
    : data_(static_cast<std::byte*>(mi_malloc_aligned(bytes, alignment))) {
    if (!data_) throw std::bad_alloc{};
    // Start the byte array's lifetime in the custom allocator's raw storage.
    data_ = ::new (data_) std::byte[bytes];
}
Buffer::~Buffer() { mi_free(data_); }

void verify_cycle(Node* start, const Buffer& buffer, std::size_t bytes, std::size_t stride) {
    const auto nodes = bytes / stride;
    std::vector<unsigned char> visited(nodes, 0);
    Node* current = start;
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.data());
    for (std::size_t i = 0; i < nodes; ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(current);
        if (address < base || address - base >= bytes || (address - base) % stride)
            throw std::runtime_error("Pointer chain leaves its allocated nodes");
        const auto index = (address - base) / stride;
        if (visited[index]) throw std::runtime_error("Pointer chain closes too early");
        visited[index] = 1;
        current = current->next;
    }
    if (current != start) throw std::runtime_error("Pointer chain is not one complete cycle");
}

Node* build_cycle(Buffer& buffer, std::size_t bytes, std::size_t stride, std::uint64_t seed) {
    const auto nodes = bytes / stride;
    std::vector<std::size_t> order(nodes);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::mt19937_64 rng(seed);
    // Explicit rejection-sampled Fisher-Yates gives the same permutation on
    // all standard libraries, without modulo bias or a second random stream.
    for (std::size_t i = nodes - 1; i > 0; --i) {
        const auto bound = static_cast<std::uint64_t>(i + 1);
        const auto threshold = (std::uint64_t{0} - bound) % bound;
        std::uint64_t random;
        do { random = rng(); } while (random < threshold);
        std::swap(order[i], order[random % bound]);
    }
    const auto node_at = [&](std::size_t i) {
        return reinterpret_cast<Node*>(buffer.data() + i * stride);
    };
    // Start all object lifetimes before linking; avoids pointer arithmetic on
    // a fictitious Node array when the runtime stride exceeds sizeof(Node).
    for (std::size_t i = 0; i < nodes; ++i) ::new (node_at(i)) Node{nullptr};
    for (std::size_t i = 0; i < nodes; ++i)
        node_at(order[i])->next = node_at(order[(i + 1) % nodes]);
    Node* start = node_at(order[0]);
    verify_cycle(start, buffer, bytes, stride);
    return start; // permutation and visited storage freed before timing
}

Measurement measure_latency(std::size_t bytes, const Config& c) {
    Buffer buffer(bytes, std::max<std::size_t>(64, c.stride));
    Node* start = build_cycle(buffer, bytes, c.stride, c.seed ^ bytes);
    Node* end = start;
    const auto nodes = static_cast<std::uint64_t>(bytes / c.stride);
    return sample(c, nodes, std::uint64_t{1} << 34,
        [&](std::uint64_t loads) { end = chase(start, loads); },
        [&](std::uint64_t) {
            if (end != start) throw std::runtime_error("Timed traversal failed to complete full cycles");
            pointer_sink = end;
        },
        [](std::uint64_t loads, double ns) { return ns / static_cast<double>(loads); });
}

std::array<Measurement, 3> measure_bandwidth(std::size_t bytes, const Config& c) {
    Buffer buffer(bytes, std::max<std::size_t>(64, c.stride));
    const auto words = bytes / sizeof(std::uint64_t);
    auto* data = ::new (buffer.data()) std::uint64_t[words];
    std::uint64_t expected_sum = 0;
    for (std::size_t i = 0; i < words; ++i) {
        data[i] = pattern(i);
        expected_sum += data[i];
    }
    std::array<Measurement, 3> result;
    const auto rate = [bytes](std::uint64_t passes, double ns) {
        // bytes/ns is numerically decimal GB/s: (bytes / 1e9) / (ns / 1e9).
        // Copy reads N/2 and writes N/2, so all three modes count N/pass.
        return static_cast<double>(bytes) * static_cast<double>(passes) / ns;
    };
    std::uint64_t observed_sum = 0;
    result[0] = sample(c, 1, std::uint64_t{1} << 32,
        [&](std::uint64_t passes) {
            observed_sum = 0;
            for (std::uint64_t p = 0; p < passes; ++p) observed_sum += read_pass(data, words);
        },
        [&](std::uint64_t passes) {
            if (observed_sum != expected_sum * passes) throw std::runtime_error("Read checksum mismatch");
            checksum_sink = observed_sum;
        }, rate);

    std::uint64_t last_value = 0;
    result[1] = sample(c, 1, std::uint64_t{1} << 32,
        [&](std::uint64_t passes) {
            for (std::uint64_t p = 0; p < passes; ++p) {
                last_value = 0xd1b54a32d192ed03ULL + p;
                write_pass(data, words, last_value);
            }
        },
        [&](std::uint64_t) {
            for (std::size_t i = 0; i < words; ++i)
                if (data[i] != last_value) throw std::runtime_error("Write validation failed");
            checksum_sink = last_value;
        }, rate);

    const auto half = words / 2;
    for (std::size_t i = 0; i < half; ++i) {
        data[i] = pattern(i);
        data[half+i] = ~pattern(i);
    }
    result[2] = sample(c, 1, std::uint64_t{1} << 32,
        [&](std::uint64_t passes) {
            for (std::uint64_t p = 0; p < passes; ++p) copy_pass(data + half, data, half);
        },
        [&](std::uint64_t) {
            for (std::size_t i = 0; i < half; ++i)
                if (data[i] != pattern(i) || data[half+i] != pattern(i))
                    throw std::runtime_error("Copy validation failed");
            checksum_sink = data[words-1];
        }, rate);
    return result;
}

std::vector<Point> run(const Config& c) {
    const auto sizes = working_sets(c);
    const auto peak = c.max_size + c.max_size / c.stride * (sizeof(std::size_t) + 1);
    std::cout << "Single thread; " << sizes.size() << " footprints; " << c.repetitions
              << " repetitions; target " << c.sample_ms << " ms/sample; stride " << c.stride
              << " B\nBenchmark allocation peak <= " << size_label(peak)
              << " (excludes executable/runtime); read/write N B/pass; copy N/2 read + N/2 write\n"
              << "Footprint       Latency ns    Read GB/s   Write GB/s    Copy GB/s\n";
    std::vector<Point> points;
    for (auto bytes : sizes) {
        Point p;
        p.bytes = bytes;
        p.latency = measure_latency(bytes, c);
        p.bandwidth = measure_bandwidth(bytes, c);
        std::cout << std::setw(12) << size_label(bytes) << std::fixed << std::setprecision(2)
                  << std::setw(13) << p.latency.value.median;
        for (const auto& bw : p.bandwidth) std::cout << std::setw(13) << bw.value.median;
        std::cout << "  [loads " << p.latency.min_operations << ".." << p.latency.max_operations
                  << "; passes R/W/C ";
        for (unsigned mode = 0; mode < 3; ++mode) {
            if (mode) std::cout << '/';
            std::cout << p.bandwidth[mode].min_operations << ".." << p.bandwidth[mode].max_operations;
        }
        std::cout << "]" << std::endl;
        points.push_back(p);
    }
    return points;
}
} // namespace cb
