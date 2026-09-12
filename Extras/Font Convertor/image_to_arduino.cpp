// image_to_arduino.cpp
//
// Convert one or more black & white images (PNG, BMP, JPG, GIF, TIFF --
// anything GDI+ can load) into a 32x32 monochrome bitmap array, in the
// SAME format/layout as font_to_arduino's font32x32[][32]:
//   extern const uint32_t graphics32x32[][32] PROGMEM = { ... };
//
// Each image becomes one 32x32 entry (auto-resized if it isn't already
// exactly 32x32). Because the array format carries no metadata about what
// each entry "is" (see font_to_arduino notes), the only record of which
// entry is which graphic is the // comment above each one and the ORDER
// they were given in -- your sketch has to index into the array itself.
//
// Uses GDI+ (built into Windows), same as font_to_arduino -- no external
// dependencies.
//
// Build (from a "Developer Command Prompt for VS"):
//   cl /EHsc /std:c++17 image_to_arduino.cpp
//
// Usage:
//   image_to_arduino.exe <output.h> <image1> [image2] [image3] ... [threshold] [invert]
//
// Examples:
//   image_to_arduino.exe icons.h star.png
//   image_to_arduino.exe icons.h star.png heart.png arrow.png
//   image_to_arduino.exe icons.h star.png 100
//   image_to_arduino.exe icons.h star.png invert
//
// Drag & drop:
//   Select one or more image files in Explorer and drag them onto
//   image_to_arduino.exe. No output filename needed -- it writes
//   "graphics.h" next to the first image, one array entry per image,
//   named in the header comment after each file's name.
//
// threshold (optional, default 128, range 0-255): luminance cutoff. Pixels
//   darker than this become "on" (lit) pixels; lighter become "off". This
//   matches the common case of a black shape drawn on a white/transparent
//   background in Photoshop.
// invert (optional): flip on/off, for images that are the opposite way
//   round (a light shape on a dark background).

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cwctype>
#include <filesystem>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

static const int GLYPH = 32; // output size (32x32), matches font_to_arduino

using Grid32 = std::vector<std::vector<uint8_t>>; // [row][col] of 0/1

// True when this process is running in a console window Windows created
// just for it (double-click / drag & drop) rather than one inherited from
// an existing terminal -- see the matching helper in font_to_arduino.cpp
// for the full explanation.
static bool HasOwnFreshConsole() {
    DWORD procs[2];
    DWORD count = GetConsoleProcessList(procs, 2);
    return count <= 1;
}

// Pack a 32x32 grid into 32 Arduino-style hex row strings, MSB-left
// (bit 31 = leftmost pixel) -- same convention as font_to_arduino.
static std::vector<std::string> GridToArduinoRows(const Grid32& grid) {
    std::vector<std::string> rows;
    rows.reserve(GLYPH);
    for (int y = 0; y < GLYPH; y++) {
        uint32_t value = 0;
        for (int x = 0; x < GLYPH; x++) {
            if (grid[y][x]) value |= (1u << (31 - x));
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", value);
        rows.push_back(buf);
    }
    return rows;
}

// Loads one image, resizes it to 32x32 if needed, and thresholds it to a
// 0/1 grid. A pixel counts as "on" when its luminance is BELOW threshold
// (i.e. dark pixels are ink by default) unless invert is set, in which
// case light pixels are ink instead. Fully/mostly transparent pixels are
// always treated as background ("off"), regardless of their RGB value.
static bool LoadAsGrid(const std::wstring& path, int threshold, bool invert, Grid32& outGrid) {
    Bitmap src(path.c_str());
    if (src.GetLastStatus() != Ok) {
        fwprintf(stderr, L"  Could not load image: %s\n", path.c_str());
        return false;
    }

    UINT w = src.GetWidth(), h = src.GetHeight();

    // Resize to exactly 32x32 if it isn't already. Bicubic keeps edges
    // reasonably clean; for crisp pixel-art results, save your source
    // image at exactly 32x32 in Photoshop so no resampling happens at all.
    Bitmap* working = &src;
    Bitmap resized(GLYPH, GLYPH, PixelFormat32bppARGB);
    if (w != (UINT)GLYPH || h != (UINT)GLYPH) {
        Graphics rg(&resized);
        rg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        rg.Clear(Color(255, 255, 255, 255)); // white background behind any transparency
        rg.DrawImage(&src, 0, 0, GLYPH, GLYPH);
        working = &resized;
    }

    outGrid.assign(GLYPH, std::vector<uint8_t>(GLYPH, 0));

    BitmapData bd;
    Rect rect(0, 0, GLYPH, GLYPH);
    if (working->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) {
        fwprintf(stderr, L"  Could not read pixels from: %s\n", path.c_str());
        return false;
    }
    for (int y = 0; y < GLYPH; y++) {
        uint8_t* row = (uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
        for (int x = 0; x < GLYPH; x++) {
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            uint8_t a = row[x * 4 + 3];

            bool on;
            if (a < 128) {
                on = false; // treat transparent pixels as background
            } else {
                int luminance = (int)std::lround(0.299 * r + 0.587 * g + 0.114 * b);
                on = luminance < threshold;
            }
            if (invert) on = !on;
            outGrid[y][x] = on ? 1 : 0;
        }
    }
    working->UnlockBits(&bd);
    return true;
}

// Human-readable name for the header comment, derived from the file name:
// uppercased stem with non-alphanumeric characters turned into underscores.
static std::string GraphicName(const std::wstring& path) {
    fs::path p(path);
    std::wstring stem = p.stem().wstring();
    std::string name;
    for (wchar_t c : stem) {
        if (iswalnum(c)) name += (char)towupper(c);
        else name += '_';
    }
    if (name.empty()) name = "GRAPHIC";
    return name;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        wprintf(L"Usage: image_to_arduino.exe <output.h> <image1> [image2] ... [threshold] [invert]\n\n");
        wprintf(L"Examples:\n");
        wprintf(L"  image_to_arduino.exe icons.h star.png\n");
        wprintf(L"  image_to_arduino.exe icons.h star.png heart.png arrow.png\n");
        wprintf(L"  image_to_arduino.exe icons.h star.png 100\n");
        wprintf(L"  image_to_arduino.exe icons.h star.png invert\n\n");
        wprintf(L"threshold (optional, default 128, 0-255): luminance cutoff -- pixels\n");
        wprintf(L"  darker than this become lit/on pixels.\n");
        wprintf(L"invert (optional): flip on/off, for a light shape on a dark background.\n\n");
        wprintf(L"Drag & drop: drop one or more image files onto image_to_arduino.exe.\n");
        wprintf(L"No output filename needed -- writes 'graphics.h' next to the first image.\n");
        if (HasOwnFreshConsole()) {
            wprintf(L"\nPress Enter to exit...");
            std::getchar();
        }
        return 1;
    }

    std::vector<std::wstring> args(argv + 1, argv + argc);

    std::wstring outputPath;
    bool haveOutput = false;
    int threshold = 128;
    bool invert = false;
    std::vector<std::wstring> images;

    for (auto& a : args) {
        std::wstring lower = a;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

        if (lower == L"invert") { invert = true; continue; }

        bool isNumber = !a.empty() && std::all_of(a.begin(), a.end(), ::iswdigit);
        if (isNumber) {
            threshold = std::clamp((int)std::wcstol(a.c_str(), nullptr, 10), 0, 255);
            continue;
        }

        fs::path p(a);
        std::wstring ext = p.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".h" && !haveOutput) {
            outputPath = a;
            haveOutput = true;
            continue;
        }

        images.push_back(a);
    }

    if (images.empty()) {
        fwprintf(stderr, L"No image files given.\n");
        if (HasOwnFreshConsole()) { wprintf(L"\nPress Enter to exit..."); std::getchar(); }
        return 1;
    }

    if (!haveOutput) {
        fs::path p(images[0]);
        outputPath = (p.parent_path() / L"graphics.h").wstring();
    }

    wprintf(L"Output: %s\n", outputPath.c_str());
    wprintf(L"Threshold: %d%s\n", threshold, invert ? L"  (inverted)" : L"");
    wprintf(L"Images: %zu\n\n", images.size());

    GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken;
    if (GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Ok) {
        fwprintf(stderr, L"Failed to initialize GDI+\n");
        return 1;
    }

    int exitCode = 0;
    {
        std::vector<Grid32> grids;
        std::vector<std::string> names;
        for (auto& img : images) {
            wprintf(L"  Reading %s...\n", img.c_str());
            Grid32 grid;
            if (!LoadAsGrid(img, threshold, invert, grid)) {
                exitCode = 1;
                continue;
            }
            grids.push_back(std::move(grid));
            names.push_back(GraphicName(img));
        }

        if (grids.empty()) {
            fwprintf(stderr, L"No images were converted successfully.\n");
            GdiplusShutdown(gdiToken);
            if (HasOwnFreshConsole()) { wprintf(L"\nPress Enter to exit..."); std::getchar(); }
            return 1;
        }

        std::ofstream out(outputPath);
        if (!out) {
            fwprintf(stderr, L"Could not open output file: %s\n", outputPath.c_str());
            GdiplusShutdown(gdiToken);
            if (HasOwnFreshConsole()) { wprintf(L"\nPress Enter to exit..."); std::getchar(); }
            return 1;
        }

        std::string glyphOrderDesc;
        for (size_t i = 0; i < names.size(); i++) {
            if (i) glyphOrderDesc += ", ";
            glyphOrderDesc += names[i];
        }

        out << "#pragma once\n\n";
        out << "// 32x32 bitmap graphics, converted from image file(s) by image_to_arduino.\n";
        out << "// Order: " << glyphOrderDesc << " (" << names.size() << " entries total).\n";
        out << "// This order MUST match however entries are indexed into this array in\n";
        out << "// your sketch -- if you regenerate this file with a different image list\n";
        out << "// or order, update that mapping too. There is no metadata in the array\n";
        out << "// itself that identifies which entry is which -- only this comment and\n";
        out << "// position in the array record that.\n";
        out << "// Lives in flash (PROGMEM) instead of RAM -- on ESP32 flash is\n";
        out << "// memory-mapped, so it can still be indexed directly as graphics32x32[idx][row].\n\n";

        out << "extern const uint32_t graphics32x32[][32] PROGMEM = {\n";
        for (size_t i = 0; i < grids.size(); i++) {
            out << "// " << names[i] << "\n";
            auto rows = GridToArduinoRows(grids[i]);
            out << "{";
            for (size_t r = 0; r < rows.size(); r++) {
                out << rows[r];
                if (r + 1 < rows.size()) out << ",";
            }
            out << "},\n";
        }
        out << "};\n";
        out.close();

        wprintf(L"\nWrote %zu graphic(s) to %s\n", grids.size(), outputPath.c_str());
    }

    GdiplusShutdown(gdiToken);

    if (HasOwnFreshConsole()) {
        wprintf(L"\nPress Enter to exit...");
        std::getchar();
    }

    return exitCode;
}
