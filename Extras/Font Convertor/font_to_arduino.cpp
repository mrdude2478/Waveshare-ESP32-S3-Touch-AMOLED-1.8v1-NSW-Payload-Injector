// font_to_arduino.cpp
//
// Convert a TrueType font to a 32x32 monochrome bitmap-font header for
// Arduino / ESP32, in the same layout as font.h:
//   extern const uint32_t font32x32[][32] PROGMEM = { ... };
//
// Uses GDI+ (built into Windows) to render each glyph, so there are no
// external dependencies to install -- just the Windows SDK that ships
// with Visual Studio.
//
// Build (from a "Developer Command Prompt for VS"):
//   cl /EHsc /std:c++17 font_to_arduino.cpp
//
// (gdiplus.lib, gdi32.lib and user32.lib are pulled in automatically via
//  the #pragma comment(lib, ...) lines below, so you don't need to list
//  them on the command line.)
//
// Usage:
//   font_to_arduino.exe <font_file.ttf> [output_file.h] [characters]
//
// Example (mirrors the original python invocation):
//   font_to_arduino.exe C:\Users\Alan\AppData\Local\Microsoft\Windows\Fonts\modern_lcd-7.ttf font2.h "%*?,-.=+-$#'@:!\|[]{}<>ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <filesystem>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

static const int    CANVAS    = 768;    // draw origin within the canvas
static const int    MARGIN    = 64;
static const float  FONT_SIZE = 192.0f; // em size used for rendering (UnitPixel) --
                                         // rendered large and then downscaled, so
                                         // GDI+'s hinting/grid-fitting (which is most
                                         // visible at small sizes) has much less
                                         // effect on the final shape
static const int    GLYPH     = 32;     // output glyph size (32x32)

using Grid32 = std::vector<std::vector<uint8_t>>; // [row][col] of 0/1

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

// Human-readable name for a glyph, used in header comments (mirrors the
// get_char_name() helper in the python script).
static std::string GlyphName(wchar_t c) {
    switch (c) {
        case L' ':  return "SPACE";
        case L'-':  return "DASH";
        case L'.':  return "DOT";
        case L',':  return "COMMA";
        case L'!':  return "EXCLAMATION";
        case L'?':  return "QUESTION";
        case L'\'': return "APOSTROPHE";
        case L'"':  return "QUOTE";
        case L':':  return "COLON";
        case L';':  return "SEMICOLON";
        default: {
            std::string s = "'";
            s += WideToUtf8(std::wstring(1, c));
            s += "'";
            return s;
        }
    }
}

// Remove duplicate characters, preserving first-occurrence order.
static std::wstring Dedup(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in) {
        if (out.find(c) == std::wstring::npos) out += c;
    }
    return out;
}

static const wchar_t* kDefaultChars =
    L"%*?,-.=+$#@:!|[]{}<>0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

// True if the path has a common font file extension. Used to tell "font
// file(s) dropped onto the .exe" apart from the classic
// <font> <output.h> <chars> <scale> command-line form.
static bool LooksLikeFontFile(const std::wstring& path) {
    fs::path p(path);
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".ttf" || ext == L".otf" || ext == L".ttc" || ext == L".fon";
}

// True when this process is running in a console window that Windows
// created just for it -- i.e. it was launched by double-clicking or
// dragging a file onto the .exe in Explorer -- rather than one it
// inherited from an already-open terminal. GetConsoleProcessList()
// returns the number of processes attached to the current console; if
// this process is the only one, the console was spun up for it and will
// vanish the instant we return, so we should pause instead of letting the
// output flash by unread.
static bool HasOwnFreshConsole() {
    DWORD procs[2];
    DWORD count = GetConsoleProcessList(procs, 2);
    return count <= 1;
}

// Ink from rendering one glyph, before any scaling. Holds the cropped
// bitmap of just its ink, plus its size and position relative to the
// shared render-canvas baseline (negative/positive offsets = above/below
// baseline, e.g. the tail of a 'y' or 'g' has a positive bottomOffset).
struct GlyphInk {
    bool blank = true;
    int w = 0, h = 0;
    int topOffset = 0;
    int bottomOffset = 0;
    std::unique_ptr<Bitmap> cropped;
};

// Renders one character and returns its ink -- cropped to its bounding box,
// unscaled -- along with its position relative to the shared baseline.
// ascentPx (from the font's real metrics) positions the glyph on the
// render canvas so it isn't clipped; it does NOT determine final output
// size (see main() for how the shared scale/baseline are chosen).
//
// Pixel access uses LockBits (direct memory access) rather than
// GetPixel/SetPixel, which are far too slow in a tight per-pixel loop.
static GlyphInk RenderInk(Font& font, wchar_t ch, float ascentPx) {
    GlyphInk ink;

    Bitmap canvas(CANVAS, CANVAS, PixelFormat32bppARGB);
    Graphics g(&canvas);
    g.Clear(Color(255, 0, 0, 0));
    // AntiAlias (not AntiAliasGridFit) renders the true outline position
    // instead of snapping stems to the pixel grid -- grid-fit hinting is
    // what caused visible shape/row differences vs. the Python/FreeType
    // output at small sizes.
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    SolidBrush white(Color(255, 255, 255, 255));

    // GDI+ DrawString's origin is the top-left of the text layout box, not
    // the baseline -- so to anchor every glyph's baseline at the same
    // canvas row (baselineY), shift the origin up by the font's ascent.
    const int baselineY = MARGIN + (int)std::ceil(ascentPx);
    PointF origin((REAL)MARGIN, (REAL)(baselineY - ascentPx));
    g.DrawString(&ch, 1, &font, origin, &white);

    // Find the ink bounding box by scanning the whole canvas. For
    // PixelFormat32bppARGB, each pixel's 4 bytes in memory are B,G,R,A.
    int xmin = CANVAS, xmax = -1, ymin = CANVAS, ymax = -1;
    {
        BitmapData bd;
        Rect fullRect(0, 0, CANVAS, CANVAS);
        canvas.LockBits(&fullRect, ImageLockModeRead, PixelFormat32bppARGB, &bd);
        for (int y = 0; y < CANVAS; y++) {
            uint8_t* row = (uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
            for (int x = 0; x < CANVAS; x++) {
                uint8_t r = row[x * 4 + 2];
                if (r > 128) {
                    if (x < xmin) xmin = x;
                    if (x > xmax) xmax = x;
                    if (y < ymin) ymin = y;
                    if (y > ymax) ymax = y;
                }
            }
        }
        canvas.UnlockBits(&bd);
    }

    if (xmax < 0) return ink; // blank glyph (e.g. space)

    ink.blank = false;
    ink.w = xmax - xmin + 1;
    ink.h = ymax - ymin + 1;
    ink.topOffset = ymin - baselineY;
    ink.bottomOffset = ymax - baselineY;
    ink.cropped.reset(canvas.Clone(xmin, ymin, ink.w, ink.h, PixelFormat32bppARGB));
    return ink;
}

// Scales one glyph's ink by the SHARED cellScale and places it in a 32x32
// grid. Baseline-anchored glyphs (letters, digits, '.', ',') are placed
// relative to the SHARED baselineRow, same as they'd sit in real text.
// Other symbols (+, =, %, brackets, etc.) don't have a natural baseline
// relationship to letters, so they're instead centered vertically in the
// cell when centerVertically is true.
static Grid32 PlaceScaled(const GlyphInk& ink, double cellScale, int baselineRow, bool centerVertically) {
    Grid32 result(GLYPH, std::vector<uint8_t>(GLYPH, 0));
    if (ink.blank) return result;

    int newW = std::max(1, (int)std::lround(ink.w * cellScale));
    int newH = std::max(1, (int)std::lround(ink.h * cellScale));

    Bitmap resized(newW, newH, PixelFormat32bppARGB);
    Graphics rg(&resized);
    rg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    rg.DrawImage(ink.cropped.get(), 0, 0, newW, newH);

    int yTop = centerVertically
        ? (GLYPH - newH) / 2
        : baselineRow + (int)std::lround(ink.topOffset * cellScale);
    int xLeft = (GLYPH / 2) - newW / 2;

    int y0 = std::max(0, yTop), x0 = std::max(0, xLeft);
    int y1 = std::min(GLYPH, yTop + newH), x1 = std::min(GLYPH, xLeft + newW);
    int sy0 = y0 - yTop, sx0 = x0 - xLeft;

    if (y1 > y0 && x1 > x0) {
        BitmapData rd;
        Rect resizedRect(0, 0, newW, newH);
        resized.LockBits(&resizedRect, ImageLockModeRead, PixelFormat32bppARGB, &rd);
        for (int y = y0; y < y1; y++) {
            int sy = sy0 + (y - y0);
            uint8_t* row = (uint8_t*)rd.Scan0 + (size_t)sy * rd.Stride;
            for (int x = x0; x < x1; x++) {
                int sx = sx0 + (x - x0);
                uint8_t r = row[sx * 4 + 2];
                if (r > 128) result[y][x] = 1;
            }
        }
        resized.UnlockBits(&rd);
    }
    return result;
}

// Pack a 32x32 grid into 32 Arduino-style hex row strings, MSB-left
// (bit 31 = leftmost pixel) -- same convention as the python script.
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

// Converts one font file to one output header. charsArg == L"" means "use
// the default character set". Assumes GDI+ is already started. Returns 0
// on success, 1 on failure (after printing an error).
static int ConvertFont(const std::wstring& fontPath, const std::wstring& outputPath,
                        const std::wstring& charsArg, double scale) {
    std::wstring chars = Dedup(charsArg.empty() ? kDefaultChars : charsArg);

    wprintf(L"Font: %s\n", fontPath.c_str());
    wprintf(L"Output: %s\n", outputPath.c_str());
    wprintf(L"Converting %zu characters: %s\n", chars.size(), chars.c_str());
    wprintf(L"Scale: %g\n", scale);

    {
        // Load the font file privately (doesn't install it system-wide).
        PrivateFontCollection fontCollection;
        if (fontCollection.AddFontFile(fontPath.c_str()) != Ok) {
            fwprintf(stderr, L"Could not load font file: %s\n", fontPath.c_str());
            return 1;
        }

        int familyCount = fontCollection.GetFamilyCount();
        if (familyCount < 1) {
            fwprintf(stderr, L"No font families found in file\n");
            return 1;
        }

        std::vector<FontFamily> families(familyCount);
        int found = 0;
        fontCollection.GetFamilies(familyCount, families.data(), &found);
        FontFamily& family = families[0];

        INT style = FontStyleRegular;
        if (!family.IsStyleAvailable(FontStyleRegular)) {
            if (family.IsStyleAvailable(FontStyleBold)) style = FontStyleBold;
            else if (family.IsStyleAvailable(FontStyleItalic)) style = FontStyleItalic;
            else style = FontStyleBoldItalic;
        }

        Font font(&family, FONT_SIZE, style, UnitPixel);
        if (font.GetLastStatus() != Ok) {
            fwprintf(stderr, L"Could not create font from family\n");
            return 1;
        }

        // Derive ONE shared scale/baseline from the actual REQUESTED
        // characters (not the font's abstract ascent/descent metric, which
        // reserves extra headroom for accents and descenders that may not
        // even be in your character set, and would otherwise make letters
        // render smaller than they need to). ascentPx (from the font's real
        // metrics) is only used to position each glyph on the render canvas
        // so it isn't clipped -- it does not set the final output size.
        UINT16 emUnits     = family.GetEmHeight(style);
        UINT16 ascentUnits = family.GetCellAscent(style);
        float ascentPx = FONT_SIZE * (float)ascentUnits / (float)emUnits;

        // First pass: render every glyph's ink.
        std::vector<GlyphInk> inks;
        inks.reserve(chars.size());
        for (wchar_t c : chars) {
            wprintf(L"  Rendering '%lc'...\n", c);
            inks.push_back(RenderInk(font, c, ascentPx));
        }

        // Use the tallest NON-descending glyph (one that doesn't dip below
        // the baseline, e.g. a capital letter or digit) as the reference
        // height -- that's what should fill the available box, same as it
        // would in the font itself.
        const int DESCENDER_TOL = 2; // px at render resolution
        int referenceHeight = 0;
        for (auto& ink : inks) {
            if (!ink.blank && ink.bottomOffset <= DESCENDER_TOL) {
                referenceHeight = std::max(referenceHeight, ink.h);
            }
        }
        if (referenceHeight == 0) {
            // Fallback: every requested glyph is a descender (unusual) --
            // use the tallest glyph overall instead.
            for (auto& ink : inks) {
                if (!ink.blank) referenceHeight = std::max(referenceHeight, ink.h);
            }
        }
        if (referenceHeight == 0) referenceHeight = (int)(FONT_SIZE / 2); // all requested glyphs blank

        // Reserve room below the baseline for whatever actually dips there.
        // It's not just lowercase descenders (g/y/p/q/j) -- brackets,
        // braces, '%', '$', '!', '?' and ',' commonly extend a little
        // below the baseline by design too. Sizing purely off
        // referenceHeight (as if nothing dipped below it) is what clipped
        // those off at the bottom of the 32px grid.
        int maxDescent = 0;
        for (auto& ink : inks) {
            if (!ink.blank) maxDescent = std::max(maxDescent, ink.bottomOffset);
        }
        maxDescent = std::max(0, maxDescent);

        const int TOP_PAD = 2;
        const int BOTTOM_PAD = 2;
        const int available = GLYPH - TOP_PAD - BOTTOM_PAD; // total px span for referenceHeight + maxDescent
        double cellScale = scale * (double)available / (referenceHeight + maxDescent);
        int baselineRow = TOP_PAD + (int)std::lround(referenceHeight * cellScale);

        // Second pass: scale each glyph by the shared cellScale and place
        // it. Letters, digits, and '.'/',' sit on the baseline like they
        // would in real text. Everything else (+, =, %, brackets, etc.)
        // has no natural baseline relationship to letters, so it's
        // centered vertically in the cell instead.
        std::vector<Grid32> glyphs;
        glyphs.reserve(chars.size());
        for (size_t i = 0; i < chars.size(); i++) {
            wchar_t c = chars[i];
            bool baselineAnchored = std::iswalnum(c) || c == L'.' || c == L',';
            glyphs.push_back(PlaceScaled(inks[i], cellScale, baselineRow, !baselineAnchored));
        }

        // Derive the font name / filename for the header comment.
        fs::path p(fontPath);
        std::string fontBaseName = p.stem().string();
        std::string fontFileName = p.filename().string();
        std::transform(fontBaseName.begin(), fontBaseName.end(), fontBaseName.begin(), ::toupper);

        std::string glyphOrderDesc;
        for (size_t i = 0; i < chars.size(); i++) {
            if (i) glyphOrderDesc += ", ";
            glyphOrderDesc += GlyphName(chars[i]);
        }

        std::ofstream out(outputPath);
        if (!out) {
            fwprintf(stderr, L"Could not open output file: %s\n", outputPath.c_str());
            return 1;
        }

        out << "#pragma once\n\n";
        out << "// " << fontBaseName << " style 32x32 bitmap font, converted from " << fontFileName << ".\n";
        out << "// Glyph order: " << glyphOrderDesc << " (" << chars.size() << " glyphs total).\n";
        out << "// This order MUST match however characters are indexed into this\n";
        out << "// array in your sketch -- if you add/remove/reorder glyphs here,\n";
        out << "// update that mapping too.\n";
        out << "// Lives in flash (PROGMEM) instead of RAM -- on ESP32 flash is\n";
        out << "// memory-mapped, so it can still be indexed directly as font32x32[idx][row].\n\n";

        out << "extern const uint32_t font32x32[][32] PROGMEM = {\n";
        for (size_t i = 0; i < chars.size(); i++) {
            out << "// " << GlyphName(chars[i]) << "\n";
            auto rows = GridToArduinoRows(glyphs[i]);
            out << "{";
            for (size_t r = 0; r < rows.size(); r++) {
                out << rows[r];
                if (r + 1 < rows.size()) out << ",";
            }
            out << "},\n";
        }
        out << "};\n";
        out.close();

        wprintf(L"\nFont converted successfully to %s\n", outputPath.c_str());
        wprintf(L"Total characters: %zu\n", chars.size());
    }

    return 0;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        wprintf(L"Usage: font_to_arduino.exe <font_file.ttf> [output_file.h] [characters] [scale]\n\n");
        wprintf(L"Examples:\n");
        wprintf(L"  font_to_arduino.exe Arial.ttf arial_font.h\n");
        wprintf(L"  font_to_arduino.exe Arial.ttf arial_font.h \"%*?,-.=+$#@:!|[]{}<>0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\"\n");
        wprintf(L"  font_to_arduino.exe Arial.ttf arial_font.h \"%*?,-.=+$#@:!|[]{}<>0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\" 1.15\n\n");
        wprintf(L"scale (optional, default 1.0): overall size multiplier for every glyph.\n");
        wprintf(L"  >1.0 makes glyphs bigger (may start clipping at the edges if pushed too far),\n");
        wprintf(L"  <1.0 makes glyphs smaller with more padding around them.\n\n");
        wprintf(L"Drag & drop: you can also drag one or more .ttf/.otf font files onto\n");
        wprintf(L"font_to_arduino.exe in Explorer. Each dropped font is converted with the\n");
        wprintf(L"default character set/scale, writing '<FontName>.h' next to the font.\n");
        if (HasOwnFreshConsole()) {
            wprintf(L"\nPress Enter to exit...");
            std::getchar();
        }
        return 1;
    }

    GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken;
    if (GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Ok) {
        fwprintf(stderr, L"Failed to initialize GDI+\n");
        return 1;
    }

    int exitCode = 0;

    // Drag-and-drop mode: if EVERY argument on the command line is itself
    // an existing font file, treat this as one or more fonts dropped onto
    // the .exe at once, and convert each with default settings. The
    // classic  font.ttf output.h "chars" scale  form is distinguished
    // from this because argv[2]/[3]/[4] in that form are essentially never
    // themselves paths to existing font files on disk.
    bool dragAndDrop = true;
    for (int i = 1; i < argc; i++) {
        if (!fs::exists(argv[i]) || !LooksLikeFontFile(argv[i])) {
            dragAndDrop = false;
            break;
        }
    }

    if (dragAndDrop) {
        for (int i = 1; i < argc; i++) {
            std::wstring fontPath = argv[i];
            fs::path p(fontPath);
            std::wstring outputPath = (p.parent_path() / p.stem()).wstring() + L".h";
            if (argc > 2) wprintf(L"\n=== %s ===\n", fontPath.c_str());
            if (ConvertFont(fontPath, outputPath, L"", 1.0) != 0) exitCode = 1;
        }
    } else {
        std::wstring fontPath   = argv[1];
        std::wstring outputPath = argc > 2 ? argv[2] : L"output_font.h";
        std::wstring charsArg   = argc > 3 ? argv[3] : L"";
        double scale = argc > 4 ? std::wcstod(argv[4], nullptr) : 1.0;
        exitCode = ConvertFont(fontPath, outputPath, charsArg, scale);
    }

    GdiplusShutdown(gdiToken);

    // If Windows created this console window just for us (double-click or
    // drag & drop onto the .exe), it will close the instant we return,
    // before anyone can read the output -- so pause here instead. When
    // launched from an existing terminal (cmd.exe, PowerShell, etc.) this
    // is skipped so scripted/CI use isn't left hanging on a keypress.
    if (HasOwnFreshConsole()) {
        wprintf(L"\nPress Enter to exit...");
        std::getchar();
    }

    return exitCode;
}
