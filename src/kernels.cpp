#include "bench.hpp"

namespace cb {
Node* chase(Node* current, std::uint64_t loads) {
    // Eight loads per loop, all on the SAME dependency chain. No extra stream,
    // index multiplication, checksum work, or clock call between these loads.
    const auto next = [](Node* p) { return static_cast<volatile Node*>(p)->next; };
    while (loads >= 8) {
        current = next(current); current = next(current);
        current = next(current); current = next(current);
        current = next(current); current = next(current);
        current = next(current); current = next(current);
        loads -= 8;
    }
    while (loads--) current = next(current);
    return current;
}

std::uint64_t read_pass(const std::uint64_t* data, std::size_t words) {
    // Independent unsigned accumulators avoid one scalar reduction bottleneck;
    // overflow is defined modulo 2^64. Every word contributes to the result.
    std::uint64_t a=0, b=0, c=0, d=0, e=0, f=0, g=0, h=0;
    std::size_t i = 0;
    for (; i + 8 <= words; i += 8) {
        a += data[i]; b += data[i+1]; c += data[i+2]; d += data[i+3];
        e += data[i+4]; f += data[i+5]; g += data[i+6]; h += data[i+7];
    }
    for (; i < words; ++i) a += data[i];
    return a+b+c+d+e+f+g+h;
}

void write_pass(std::uint64_t* data, std::size_t words, std::uint64_t value) {
    for (std::size_t i = 0; i < words; ++i) data[i] = value;
}

void copy_pass(std::uint64_t* destination, const std::uint64_t* source, std::size_t words) {
    for (std::size_t i = 0; i < words; ++i) destination[i] = source[i];
}
} // namespace cb
