#include <iostream>
#include <fstream>
#include <vector>
#include <zlib.h>
#include <string>
#include <cstring>
#include <iomanip>
#include <algorithm>

// Function to compress a file
bool compressFile(const std::string& inputFile, 
                 const std::string& outputFile,
                 int compressionLevel = Z_BEST_COMPRESSION,
                 int bufferSize = 1024 * 1024) {
    
    std::ifstream in(inputFile, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "Error: Could not open input file " << inputFile << std::endl;
        return false;
    }

    std::string mode = "wb" + std::to_string(compressionLevel);
    gzFile out = gzopen(outputFile.c_str(), mode.c_str());
    if (!out) {
        std::cerr << "Error: Could not open output file " << outputFile << std::endl;
        in.close();
        return false;
    }

    gzbuffer(out, bufferSize);
    std::vector<char> buffer(bufferSize);

    while (!in.eof()) {
        in.read(buffer.data(), buffer.size());
        std::streamsize bytesRead = in.gcount();

        if (bytesRead > 0) {
            if (gzwrite(out, buffer.data(), static_cast<unsigned>(bytesRead)) == 0) {
                std::cerr << "Error: Failed to write compressed data" << std::endl;
                gzclose(out);
                in.close();
                return false;
            }
        }
    }

    in.close();
    return (gzclose(out) == Z_OK);
}

// Function to decompress a file
bool decompressFile(const std::string& inputFile, 
                   const std::string& outputFile,
                   int bufferSize = 1024 * 1024) {
    
    gzFile in = gzopen(inputFile.c_str(), "rb");
    if (!in) {
        std::cerr << "Error: Could not open input file " << inputFile << std::endl;
        return false;
    }

    std::ofstream out(outputFile, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "Error: Could not open output file " << outputFile << std::endl;
        gzclose(in);
        return false;
    }

    std::vector<char> buffer(bufferSize);
    int bytesRead;
    while ((bytesRead = gzread(in, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
        out.write(buffer.data(), bytesRead);
    }

    out.close();
    bool success = (gzclose(in) == Z_OK);
    
    if (bytesRead < 0) {
        std::cerr << "Error: Failed to read compressed data" << std::endl;
        return false;
    }
    
    return success;
}

// Custom clamp function
template<typename T>
T clamp(T value, T min, T max) {
    return (value < min) ? min : (value > max) ? max : value;
}

void printHelp() {
    std::cout << "ESP32 Web File Compressor/Decompressor\n";
    std::cout << "Usage: webcompressor [options] input_file [output_file]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -c          Compress mode (default)\n";
    std::cout << "  -d          Decompress mode\n";
    std::cout << "  -l LEVEL    Compression level (1-9, default: 9)\n";
    std::cout << "  -b SIZE     Buffer size in KB (default: 1024)\n";
    std::cout << "  -f          Force overwrite of output file\n";
    std::cout << "  -h          Show this help message\n\n";
    std::cout << "If output_file is omitted:\n";
    std::cout << "  - Compression adds .gz extension\n";
    std::cout << "  - Decompression removes .gz extension\n";
}

int main(int argc, char* argv[]) {
    // Default settings
    bool compressMode = true;
    int compressionLevel = Z_BEST_COMPRESSION;
    int bufferSizeKB = 1024;
    bool forceOverwrite = false;
    std::string inputFile, outputFile;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            printHelp();
            return 0;
        }
        else if (strcmp(argv[i], "-d") == 0) {
            compressMode = false;
        }
        else if (strcmp(argv[i], "-c") == 0) {
            compressMode = true;
        }
        else if (strcmp(argv[i], "-l") == 0 && i+1 < argc) {
            compressionLevel = clamp(atoi(argv[++i]), 1, 9);
        }
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) {
            bufferSizeKB = std::max(1, atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-f") == 0) {
            forceOverwrite = true;
        }
        else if (inputFile.empty()) {
            inputFile = argv[i];
        }
        else if (outputFile.empty()) {
            outputFile = argv[i];
        }
        else {
            std::cerr << "Error: Too many arguments\n";
            printHelp();
            return 1;
        }
    }

    // Validate input
    if (inputFile.empty()) {
        std::cerr << "Error: No input file specified\n";
        printHelp();
        return 1;
    }

    // Set default output filename if not specified
    if (outputFile.empty()) {
        if (compressMode) {
            outputFile = inputFile + ".gz";
        } else {
            if (inputFile.size() > 3 && 
                inputFile.substr(inputFile.size() - 3) == ".gz") {
                outputFile = inputFile.substr(0, inputFile.size() - 3);
            } else {
                outputFile = inputFile + ".decompressed";
            }
        }
    }

    // Check if output file exists
    if (!forceOverwrite) {
        std::ifstream test(outputFile);
        if (test.good()) {
            std::cerr << "Error: Output file exists (use -f to overwrite)\n";
            return 1;
        }
    }

    // Perform operation
    bool success;
    if (compressMode) {
        std::cout << "Compressing " << inputFile << " to " << outputFile << "\n";
        std::cout << "Settings: Level=" << compressionLevel;
        std::cout << ", Buffer=" << bufferSizeKB << "KB\n";
        success = compressFile(inputFile, outputFile, compressionLevel, bufferSizeKB * 1024);
    } else {
        std::cout << "Decompressing " << inputFile << " to " << outputFile << "\n";
        success = decompressFile(inputFile, outputFile, bufferSizeKB * 1024);
    }

    if (success) {
        std::cout << "Operation successful!\n";
        
        // Show file sizes
        std::ifstream in(inputFile, std::ios::binary | std::ios::ate);
        std::ifstream out(outputFile, std::ios::binary | std::ios::ate);
        
        if (in.good() && out.good()) {
            size_t inputSize = in.tellg();
            size_t outputSize = out.tellg();
            
            std::cout << "Input: " << inputSize << " bytes\n";
            std::cout << "Output: " << outputSize << " bytes\n";
            
            if (compressMode) {
                double ratio = (1.0 - (double)outputSize/inputSize) * 100.0;
                std::cout << "Reduction: " << std::fixed << std::setprecision(1) << ratio << "%\n";
            }
        }
        return 0;
    } else {
        std::cerr << "Operation failed!\n";
        return 1;
    }
}