
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <regex>

void exportToHeader(const std::string& inputFile, const std::string& outputFile, const std::string& arrayName) {
    std::ifstream in(inputFile, std::ios::binary);
    if (!in) {
        std::cerr << "Error opening input file: " << inputFile << "\n";
        return;
    }

    std::ofstream out(outputFile);
    if (!out) {
        std::cerr << "Error opening output file: " << outputFile << "\n";
        return;
    }

    out << "#pragma once\n";
    out << "#include <stdint.h>\n";
    out << "#include <pgmspace.h>\n\n";
    out << "const uint8_t " << arrayName << "[] PROGMEM = {\n    ";

    size_t count = 0;
    char byte;
    while (in.get(byte)) {
        out << "0x" << std::hex << std::setw(2) << std::setfill('0') << (static_cast<unsigned>(byte) & 0xFF);
        if (in.peek() != EOF) out << ", ";
        if (++count % 16 == 0) out << "\n    ";
    }

    out << "\n};\n";
    out << "const unsigned int " << arrayName << "_len = sizeof(" << arrayName << ");\n";

    std::cout << "✔ Header file generated: " << outputFile << "\n";
}

void importFromHeader(const std::string& inputFile, const std::string& outputFile) {
    std::ifstream in(inputFile);
    if (!in) {
        std::cerr << "Error opening input header file.\n";
        return;
    }

    std::ofstream out(outputFile, std::ios::binary);
    if (!out) {
        std::cerr << "Error opening output binary file.\n";
        return;
    }

    std::string line;
    std::regex hexByteRegex(R"(0x[0-9a-fA-F]{2})");

    while (std::getline(in, line)) {
        for (std::sregex_iterator i(line.begin(), line.end(), hexByteRegex), end; i != end; ++i) {
            unsigned int byte;
            std::stringstream ss((*i).str());
            ss >> std::hex >> byte;
            out.put(static_cast<char>(byte));
        }
    }

    std::cout << "✔ Binary file recovered: " << outputFile << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n"
                  << "  To export: " << argv[0] << " export <input.bin> <output.h> <array_name>\n"
                  << "  To import: " << argv[0] << " import <input.h> <output.bin>\n";
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "export" && argc == 5) {
        exportToHeader(argv[2], argv[3], argv[4]);
    } else if (mode == "import" && argc == 4) {
        importFromHeader(argv[2], argv[3]);
    } else {
        std::cerr << "Invalid arguments.\n";
        return 1;
    }

    return 0;
}
