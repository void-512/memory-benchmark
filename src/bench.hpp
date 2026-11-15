#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cb {
constexpr std::size_t KiB = 1024;
constexpr std::size_t MiB = KiB * KiB;
constexpr std::size_t GiB = MiB * KiB;

struct Config {
    std::size_t min_size = 4 * KiB;
    std::size_t max_size = 512 * MiB;
    std::size_t stride = 64;
    std::size_t memory_limit = GiB;
    unsigned density = 4; // intervals per octave
    unsigned repetitions = 5;
    double sample_ms = 20;
    std::uint64_t seed = 20261114;
    std::filesystem::path output = "results";
};
Config parse_config(int argc, char** argv);
void validate_config(const Config& config);
std::size_t parse_size(const std::string& text);
std::vector<std::size_t> working_sets(const Config& config);
std::string size_label(std::size_t bytes);
const char* usage();

struct Stats {
    double median = 0;
    double minimum = 0;
    double maximum = 0;
};
Stats aggregate(std::vector<double> samples);

struct Measurement {
    Stats value;
    std::uint64_t min_operations = 0;
    std::uint64_t max_operations = 0;
};
struct Point {
    std::size_t bytes = 0;
    Measurement latency;
    std::array<Measurement, 3> bandwidth; // read, write, copy
};

// A node's pointer is loaded through a volatile glvalue in chase(). The node
// itself has normal storage, with its lifetime started by placement new.
struct Node { Node* next; };
Node* chase(Node* current, std::uint64_t loads);
std::uint64_t read_pass(const std::uint64_t* data, std::size_t words);
void write_pass(std::uint64_t* data, std::size_t words, std::uint64_t value);
void copy_pass(std::uint64_t* destination, const std::uint64_t* source, std::size_t words);

class Buffer {
public:
    Buffer(std::size_t bytes, std::size_t alignment);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    std::byte* data() const { return data_; }
private:
    std::byte* data_;
};
Node* build_cycle(Buffer& buffer, std::size_t bytes, std::size_t stride, std::uint64_t seed);
void verify_cycle(Node* start, const Buffer& buffer, std::size_t bytes, std::size_t stride);
Measurement measure_latency(std::size_t bytes, const Config& config);
std::array<Measurement, 3> measure_bandwidth(std::size_t bytes, const Config& config);
std::vector<Point> run(const Config& config);
void render(const std::vector<Point>& points, const Config& config);
} // namespace cb
