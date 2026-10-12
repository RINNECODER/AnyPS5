#include <nid/BinaryPatcherFactory.hpp>
#include <nid/ExportExclusions.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> _readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open: " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

void _writeFile(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write: " + path);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: nid_patcher <library_name> [--preserve-exports <unpatched-library> | --preserve-export-list <export-list>] <file> [file2 ...]\n"
                     "       nid_patcher --write-export-list <unpatched-library> <export-list>\n";
        return 1;
    }

    if (std::string(argv[1]) == "--write-export-list") {
        if (argc != 4) {
            std::cerr << "FAIL: --write-export-list requires a reference library and an export list\n";
            return 2;
        }
        try {
            std::cout << (Nid::WriteExportList(argv[2], argv[3]) ? "WROTE: " : "UNCHANGED: ") << argv[3] << "\n";
        } catch (const std::exception& e) {
            std::cerr << "FAIL: " << argv[3] << ": " << e.what() << "\n";
            return 2;
        }
        return 0;
    }

    const std::string libraryName = argv[1];
    std::unordered_set<std::string> excludedExports;
    std::vector<std::string> paths;
    try {
        bool hasReference = false;
        for (int i = 2; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--preserve-exports" || argument == "--preserve-export-list") {
                if (hasReference || i + 1 == argc) throw std::runtime_error(argument + " requires exactly one reference");
                hasReference = true;
                excludedExports = argument == "--preserve-exports" ? Nid::ReadExportExclusions(argv[++i]) : Nid::ReadExportList(argv[++i]);
            } else {
                if (argument.starts_with("--")) throw std::runtime_error("unknown option: " + argument);
                paths.push_back(argument);
            }
        }
        if (paths.empty()) throw std::runtime_error("no files to patch");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 2;
    }

    for (const std::string& path : paths) {
        try {
            auto binary = _readFile(path);
            const auto patcher = Nid::MakePatcher(binary);
            patcher->PatchNids(binary, libraryName, excludedExports);
            _writeFile(path, binary);
            std::cout << "OK: " << path << "\n";
        } catch (const std::exception& e) {
            std::cerr << "FAIL: " << path << ": " << e.what() << "\n";
            return 2;
        }
    }

    return 0;
}
