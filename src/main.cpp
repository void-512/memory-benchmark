#include "bench.hpp"

#include <exception>
#include <iostream>
#include <new>
#include <string_view>

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << cb::usage();
            return 0;
        }
        const auto config = cb::parse_config(argc, argv);
        // Check destination before spending time on measurement.
        std::filesystem::create_directories(config.output);
        const auto points = cb::run(config);
        cb::render(points, config);
        std::cout << "Wrote " << (config.output / "bandwidth.svg") << " and "
                  << (config.output / "latency.svg") << '\n';
        return 0;
    } catch (const std::bad_alloc&) {
        std::cerr << "cachebench: allocation failed; reduce --max-size\n";
    } catch (const std::exception& error) {
        std::cerr << "cachebench: " << error.what() << '\n';
    }
    return 1;
}
