#include <sufkit/sufkit.hpp>

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sufkit_example_inspect_error reference.sufidx\n";
        return 2;
    }

    try {
        const auto info = sufkit::inspect_index(argv[1]);
        std::cout << sufkit::to_string(info.kind) << '\t'
                  << info.backend << '\t'
                  << info.format_version << '\t'
                  << info.total_bases << '\n';
    } catch (const sufkit::Error& error) {
        std::cerr << sufkit::to_string(error.code()) << ": "
                  << error.what() << '\n';
        return 1;
    }
}
