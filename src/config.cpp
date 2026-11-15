#include "bench.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace cb {
namespace {
std::uint64_t integer(std::string_view value) {
    std::uint64_t number = 0;
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size() || value.empty())
        throw std::invalid_argument("Invalid unsigned integer: " + std::string(value));
    return number;
}
unsigned bounded_integer(const std::string& value) {
    const auto n = integer(value);
    if (n > std::numeric_limits<unsigned>::max()) throw std::invalid_argument("Integer too large");
    return static_cast<unsigned>(n);
}
}

const char* usage() {
    return "cachebench [options]\n"
           "  --min-size SIZE       Smallest footprint (default 4K)\n"
           "  --max-size SIZE       Largest footprint (default 512M)\n"
           "  --output DIRECTORY    Write exactly two SVG files (default results)\n"
           "  --density N           Intervals per doubling, 2..32 (default 4)\n"
           "  --repetitions N       Samples per mode, 3..31 (default 5)\n"
           "  --sample-ms N         Target sample duration, 1..1000 (default 20)\n"
           "  --stride SIZE         Node spacing, power of two 16..4096 (default 64)\n"
           "  --memory-limit SIZE   Benchmark allocation budget, <=2G (default 1G)\n"
           "  --seed N              Random-cycle seed (default 20261114)\n"
           "  --help                Show this help\n"
           "Sizes: integer bytes, or K/KiB, M/MiB, G/GiB (binary multiples).\n"
           "Footprints must be multiples of twice the stride.\n";
}

std::size_t parse_size(const std::string& text) {
    const auto end = text.find_first_not_of("0123456789");
    const auto digits = std::string_view(text).substr(0, end);
    const auto number = integer(digits);
    const std::string suffix = end == std::string::npos ? "" : text.substr(end);
    std::uint64_t multiplier = 1;
    if (suffix == "K" || suffix == "k" || suffix == "KiB") multiplier = KiB;
    else if (suffix == "M" || suffix == "m" || suffix == "MiB") multiplier = MiB;
    else if (suffix == "G" || suffix == "g" || suffix == "GiB") multiplier = GiB;
    else if (!suffix.empty() && suffix != "B") throw std::invalid_argument("Invalid size suffix: " + suffix);
    if (!number || number > std::numeric_limits<std::size_t>::max() / multiplier)
        throw std::invalid_argument("Size is zero or overflows: " + text);
    return static_cast<std::size_t>(number * multiplier);
}

Config parse_config(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (i + 1 == argc) throw std::invalid_argument("Missing value for " + option);
        const std::string value = argv[++i];
        if (option == "--min-size") c.min_size = parse_size(value);
        else if (option == "--max-size") c.max_size = parse_size(value);
        else if (option == "--output") c.output = value;
        else if (option == "--stride") c.stride = parse_size(value);
        else if (option == "--memory-limit") c.memory_limit = parse_size(value);
        else if (option == "--density") c.density = bounded_integer(value);
        else if (option == "--repetitions") c.repetitions = bounded_integer(value);
        else if (option == "--sample-ms") c.sample_ms = static_cast<double>(integer(value));
        else if (option == "--seed") c.seed = integer(value);
        else throw std::invalid_argument("Unknown option: " + option);
    }
    validate_config(c);
    return c;
}

void validate_config(const Config& c) {
    if (sizeof(void*) != 8) throw std::invalid_argument("A 64-bit target is required");
    if (c.stride < 16 || c.stride > 4096 || (c.stride & (c.stride - 1)))
        throw std::invalid_argument("Stride must be a power of two from 16 to 4096");
    if (c.min_size < 2 * c.stride || c.max_size < c.min_size || c.max_size > GiB)
        throw std::invalid_argument("Require 2*stride <= min-size <= max-size <= 1G");
    if (c.min_size % (2 * c.stride) || c.max_size % (2 * c.stride))
        throw std::invalid_argument("Size endpoints must be multiples of twice the stride");
    if (c.density < 2 || c.density > 32 || c.repetitions < 3 || c.repetitions > 31 ||
        !std::isfinite(c.sample_ms) || c.sample_ms < 1 || c.sample_ms > 1000)
        throw std::invalid_argument("Require density 2..32, repetitions 3..31, sample-ms 1..1000");
    // During construction: footprint + permutation + one visited byte/node.
    // The permutation is freed before timing. All arithmetic is bounded by 1 GiB.
    const auto nodes = c.max_size / c.stride;
    const auto peak = c.max_size + nodes * (sizeof(std::size_t) + 1);
    if (c.memory_limit > 2 * GiB || peak > c.memory_limit)
        throw std::invalid_argument("Footprint plus cycle setup exceeds --memory-limit (maximum 2G)");
    if (c.output.empty()) throw std::invalid_argument("Output directory cannot be empty");
}

std::vector<std::size_t> working_sets(const Config& c) {
    validate_config(c);
    const auto quantum = 2 * c.stride;
    std::vector<std::size_t> sizes{c.min_size};
    const double octaves = std::log2(static_cast<double>(c.max_size) / c.min_size);
    const auto steps = static_cast<unsigned>(std::ceil(octaves * c.density));
    for (unsigned i = 1; i < steps; ++i) {
        const double raw = c.min_size * std::exp2(static_cast<double>(i) / c.density);
        auto size = static_cast<std::size_t>(std::llround(raw / quantum)) * quantum;
        if (size > sizes.back() && size < c.max_size) sizes.push_back(size);
    }
    if (sizes.back() != c.max_size) sizes.push_back(c.max_size);
    return sizes;
}

std::string size_label(std::size_t bytes) {
    const auto unit = bytes >= GiB ? GiB : bytes >= MiB ? MiB : KiB;
    std::ostringstream out;
    out << std::setprecision(3) << static_cast<double>(bytes) / unit
        << (unit == GiB ? " GiB" : unit == MiB ? " MiB" : " KiB");
    return out.str();
}

Stats aggregate(std::vector<double> samples) {
    if (samples.empty()) throw std::runtime_error("No measurement samples");
    for (double v : samples)
        if (!std::isfinite(v) || v <= 0) throw std::runtime_error("Nonpositive or nonfinite measurement");
    std::sort(samples.begin(), samples.end());
    const auto middle = samples.size() / 2;
    const double median = samples.size() % 2 ? samples[middle] :
        samples[middle - 1] + (samples[middle] - samples[middle - 1]) / 2;
    return {median, samples.front(), samples.back()};
}
} // namespace cb
