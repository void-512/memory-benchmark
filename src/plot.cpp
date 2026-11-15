#include "bench.hpp"

#include "backend/BackendSVG.hpp"

#include <mimalloc.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace cb {
namespace {
using trase::BackendSVG;
using trase::RGBA;
constexpr float width = 1120, height = 720;
constexpr float left = 105, right = 1080, top = 182, bottom = 568;
const RGBA ink{32, 47, 65, 255};
const RGBA muted{83, 101, 122, 255};
const RGBA grid{220, 228, 236, 255};
const std::array<RGBA, 3> colors{{{26, 111, 190, 255}, {214, 99, 34, 255}, {24, 139, 111, 255}}};

void line(BackendSVG& svg, float x1, float y1, float x2, float y2, const RGBA& color, float thickness = 1) {
    svg.stroke_color(color);
    svg.stroke_width(thickness);
    svg.begin_path();
    svg.move_to({x1, y1});
    svg.line_to({x2, y2});
    svg.stroke();
}

// All displayed strings are controlled by this adapter; no unescaped user text
// is passed to Trase's text API (which writes literal XML character content).
void text(BackendSVG& svg, float x, float y, const std::string& value, float size = 14,
          const RGBA& color = ink, unsigned align = trase::ALIGN_LEFT) {
    svg.font_face("sans-serif");
    svg.font_size(size);
    svg.fill_color(color);
    svg.text_align(align | trase::ALIGN_MIDDLE);
    svg.text({x, y}, value.c_str(), nullptr);
}

std::string number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(4) << value;
    return out.str();
}

// Trase's init() emits an external DTD and unused tooltip JavaScript even for
// static plots. Strip only those two blocks from its generated document.
void erase_block(std::string& document, const std::string& open, const std::string& close) {
    const auto start = document.find(open);
    if (start == std::string::npos) throw std::runtime_error("Unexpected Trase SVG prolog");
    const auto end = document.find(close, start);
    if (end == std::string::npos) throw std::runtime_error("Incomplete Trase SVG prolog");
    document.erase(start, end + close.size() - start);
}

std::string draw(const std::vector<Point>& points, const Config& c, bool bandwidth) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    BackendSVG svg(out);
    const std::string title = bandwidth ? "Memory bandwidth across the working set" :
                                          "Dependent-load latency across the working set";
    svg.init({width, height}, title.c_str());
    svg.fill_color({255, 255, 255, 255});
    svg.stroke_color({255, 255, 255, 255});
    svg.rect({{0, 0}, {width, height}});
    text(svg, left, 42, title, 27);
    text(svg, left, 78, "Single thread | median of " + std::to_string(c.repetitions) +
         " samples | target " + number(c.sample_ms) + " ms/sample | node stride " +
         std::to_string(c.stride) + " B", 14, muted);
    const std::array<std::string, 3> labels{{"Sequential read", "Sequential write", "Sequential copy"}};
    for (unsigned i = 0; i < (bandwidth ? 3u : 1u); ++i) {
        const float x = left + 280 * static_cast<float>(i);
        line(svg, x, 120, x + 34, 120, colors[i], 3);
        text(svg, x + 45, 120, bandwidth ? labels[i] : "Random pointer chase", 15);
    }
    text(svg, left, 157, bandwidth ? "Effective bandwidth (GB/s, decimal)" :
                                      "Latency (ns per dependent load)", 14, muted);

    double xlow = std::log2(static_cast<double>(points.front().bytes));
    double xhigh = std::log2(static_cast<double>(points.back().bytes));
    if (xhigh == xlow) { xlow -= 0.5; xhigh += 0.5; }
    const auto xpixel = [&](std::size_t bytes) {
        return left + static_cast<float>((std::log2(static_cast<double>(bytes)) - xlow) /
                                         (xhigh - xlow)) * (right - left);
    };
    double ymax = 0;
    for (const auto& p : points) {
        if (bandwidth) for (const auto& m : p.bandwidth) ymax = std::max(ymax, m.value.median);
        else ymax = std::max(ymax, p.latency.value.median);
    }
    const double raw_step = ymax * 1.1 / 5;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw_step)));
    const double fraction = raw_step / magnitude;
    const double step = (fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 2.5 ? 2.5 :
                         fraction <= 5 ? 5 : 10) * magnitude;
    ymax = std::ceil(ymax * 1.08 / step) * step;
    const auto ypixel = [&](double value) {
        return bottom - static_cast<float>(value / ymax) * (bottom - top);
    };
    for (unsigned i = 0; i <= static_cast<unsigned>(std::llround(ymax / step)); ++i) {
        const double value = i * step;
        const float y = ypixel(value);
        line(svg, left, y, right, y, grid);
        text(svg, left - 16, y, number(value), 13, muted, trase::ALIGN_RIGHT);
    }

    std::vector<std::size_t> ticks{points.front().bytes};
    for (unsigned power = 0; power <= 30; ++power) {
        const auto bytes = std::size_t{1} << power;
        if (bytes > points.front().bytes && bytes < points.back().bytes &&
            xpixel(bytes) - xpixel(ticks.back()) >= 95 &&
            xpixel(points.back().bytes) - xpixel(bytes) >= 95) ticks.push_back(bytes);
    }
    if (points.back().bytes != ticks.back()) ticks.push_back(points.back().bytes);
    for (auto bytes : ticks) {
        const float x = xpixel(bytes);
        line(svg, x, top, x, bottom, grid);
        text(svg, x, bottom + 26, size_label(bytes), 13, muted, trase::ALIGN_CENTER);
    }
    line(svg, left, bottom, right, bottom, muted);
    line(svg, left, top, left, bottom, muted);

    // Straight segments and markers at every measured point; no smoothing.
    for (unsigned series = 0; series < (bandwidth ? 3u : 1u); ++series) {
        svg.stroke_color(colors[series]);
        svg.stroke_width(2.6f);
        svg.begin_path();
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& p = points[i];
            const auto value = bandwidth ? p.bandwidth[series].value.median : p.latency.value.median;
            const trase::vfloat2_t xy{xpixel(p.bytes), ypixel(value)};
            if (i == 0) svg.move_to(xy); else svg.line_to(xy);
        }
        svg.stroke();
        svg.fill_color(colors[series]);
        svg.stroke_width(0);
        for (const auto& p : points) {
            const auto value = bandwidth ? p.bandwidth[series].value.median : p.latency.value.median;
            svg.circle({xpixel(p.bytes), ypixel(value)}, 2.8f);
        }
    }
    text(svg, (left + right) / 2, 631, "Total active working-set footprint (logarithmic scale)",
         16, ink, trase::ALIGN_CENTER);
    text(svg, left, 671, bandwidth ?
         "Read/write: N bytes/pass. Copy: N/2 bytes read + N/2 bytes written. Cache traffic is not DRAM traffic." :
         "One randomized cycle spans N/stride nodes. Each timed sample covers complete cycles; warm-up is excluded.",
         13, muted);
    text(svg, left, 695, "Markers are measurements; lines connect adjacent samples. Frequency, TLBs and system load affect results.",
         13, muted);
    svg.finalise();
    auto document = out.str();
    erase_block(document, "<!DOCTYPE", ">");
    erase_block(document, "<script>", "</script>");
    const auto root = document.find("<svg ");
    document.insert(root + 5, "viewBox=\"0 0 1120 720\" role=\"img\" ");

    // Retain actual medians, variability and exact operation counts inside each
    // SVG for inspection/replotting, without producing a third output file.
    std::ostringstream metadata;
    metadata.imbue(std::locale::classic());
    metadata << std::setprecision(17) << "<title>" << title << "</title>\n"
             << "<metadata id=\"measurements\">\n"
             << "cachebench-1; allocator=mimalloc; mimalloc_version=" << MI_MALLOC_VERSION
             << "; clock=steady_clock; single_thread=true; seed=" << c.seed
             << "; stride=" << c.stride << "; repetitions=" << c.repetitions
             << "; target_ms=" << c.sample_ms << '\n'
             << "footprint_bytes,mode,median,min,max,unit,min_operations,max_operations\n";
    for (const auto& p : points) {
        const auto row = [&](const Measurement& m, const char* mode, const char* unit) {
            metadata << p.bytes << ',' << mode << ',' << m.value.median << ',' << m.value.minimum
                     << ',' << m.value.maximum << ',' << unit << ',' << m.min_operations
                     << ',' << m.max_operations << '\n';
        };
        if (bandwidth) {
            row(p.bandwidth[0], "read", "GB/s");
            row(p.bandwidth[1], "write", "GB/s");
            row(p.bandwidth[2], "copy", "GB/s");
        } else row(p.latency, "pointer_chase", "ns/load");
    }
    metadata << "</metadata>\n";
    document.insert(document.find('>', root) + 1, "\n" + metadata.str());
    return document;
}

void save(const std::filesystem::path& path, const std::string& document) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot open output: " + path.string());
    file << document;
    file.close();
    if (!file) throw std::runtime_error("Cannot write output: " + path.string());
}
}

void render(const std::vector<Point>& points, const Config& c) {
    if (points.empty()) throw std::runtime_error("Cannot plot an empty sweep");
    const auto bandwidth = draw(points, c, true);
    const auto latency = draw(points, c, false);
    std::filesystem::create_directories(c.output);
    save(c.output / "bandwidth.svg", bandwidth);
    save(c.output / "latency.svg", latency);
}
} // namespace cb
