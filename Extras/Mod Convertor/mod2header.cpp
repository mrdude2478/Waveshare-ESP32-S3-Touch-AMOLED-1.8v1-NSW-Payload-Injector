// mod2header.cpp
//
// Converts a binary file (e.g. a ProTracker .mod file) into a C/C++ header
// file containing a PROGMEM byte array, in the exact format expected by
// ESP32 sketches like:
//
//   extern const uint8_t song[] PROGMEM  = {
//     0x70, 0x6f, 0x70, ...
//   };
//
//   extern const size_t song_size = sizeof(song);
//
// USAGE
//   Drag and drop a .mod file onto mod2header.exe
//     -> writes "song.h" next to the .mod file, with array name "song"
//
//   Or from the command line:
//     mod2header.exe <input file> [array_name] [output.h]
//
// Build (from a "Developer Command Prompt for VS"):
//     cl /EHsc /std:c++17 mod2header.cpp
//
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

static std::string sanitizeIdentifier(std::string s) {
    if (s.empty()) return "song";
    for (char& c : s) {
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_')) c = '_';
    }
    if (isdigit(static_cast<unsigned char>(s[0]))) s = "_" + s;
    return s;
}

static bool convertFile(const fs::path& inputPath,
                         const std::string& arrayNameIn,
                         const fs::path& outputPathIn) {
    std::ifstream in(inputPath, std::ios::binary);
    if (!in) {
        std::cerr << "ERROR: could not open input file: " << inputPath << "\n";
        return false;
    }

    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    in.close();

    if (data.empty()) {
        std::cerr << "ERROR: input file is empty: " << inputPath << "\n";
        return false;
    }

    std::string arrayName = arrayNameIn.empty() ? "song" : sanitizeIdentifier(arrayNameIn);

    fs::path outputPath = outputPathIn;
    if (outputPath.empty()) {
        outputPath = inputPath.parent_path() / (arrayName + ".h");
    }

    std::ofstream out(outputPath, std::ios::binary); // binary so no '\r' gets inserted on Windows
    if (!out) {
        std::cerr << "ERROR: could not open output file for writing: " << outputPath << "\n";
        return false;
    }

    // The reference song.h files use CRLF line endings throughout, so we
    // reproduce that exactly (out is opened in binary mode, so "\r\n" is
    // written as-is on every platform, including Windows).
    out << "extern const uint8_t " << arrayName << "[] PROGMEM  = {\r\n";

    const size_t perLine = 16;
    char buf[8];
    const size_t n = data.size();
    for (size_t i = 0; i < n; ++i) {
        if (i % perLine == 0) out << "  ";

        std::snprintf(buf, sizeof(buf), "0x%02x", data[i]);
        out << buf;

        bool isLastByte = (i == n - 1);
        if (!isLastByte) out << ", ";

        bool endOfLine = (i % perLine == perLine - 1);
        if (endOfLine || isLastByte) out << "\r\n";
    }

    out << "};\r\n\r\n";
    out << "extern const size_t " << arrayName << "_size = sizeof(" << arrayName << ");\r\n";
    out.close();

    std::cout << "Wrote " << outputPath.string() << " (" << n << " bytes, array \""
               << arrayName << "\")\n";
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr <<
            "mod2header - convert a binary file into a PROGMEM C array header\n\n"
            "Usage:\n"
            "  Drag and drop one or more .mod files onto this .exe\n"
            "    -> for a single file: writes \"song.h\" (array \"song\") next to it\n"
            "    -> for multiple files: writes \"<name>.h\" (array \"<name>\") per file\n\n"
            "  Or from the command line:\n"
            "    mod2header.exe <input file> [--name=array_name] [--out=output.h]\n";
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    // Separate plain file arguments from --name=/--out= flags.
    std::vector<fs::path> files;
    std::string nameOverride;
    fs::path outOverride;
    const std::string namePrefix = "--name=";
    const std::string outPrefix = "--out=";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(namePrefix, 0) == 0) {
            nameOverride = arg.substr(namePrefix.size());
        } else if (arg.rfind(outPrefix, 0) == 0) {
            outOverride = arg.substr(outPrefix.size());
        } else {
            files.push_back(arg);
        }
    }

    if (files.empty()) {
        std::cerr << "ERROR: no input file given.\n";
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    bool ok = true;

    if (files.size() == 1) {
        // Single file: default array name is "song" unless overridden.
        std::string arrayName = nameOverride.empty() ? "song" : nameOverride;
        ok = convertFile(files[0], arrayName, outOverride);
    } else {
        // Multiple files dropped at once: name each array after its file
        // (ignoring --name/--out, which only make sense for a single file).
        for (const auto& f : files) {
            std::string arrayName = sanitizeIdentifier(f.stem().string());
            ok = convertFile(f, arrayName, fs::path()) && ok;
        }
    }

    std::cout << "\nPress Enter to exit...";
    std::cin.get();
    return ok ? 0 : 1;
}
