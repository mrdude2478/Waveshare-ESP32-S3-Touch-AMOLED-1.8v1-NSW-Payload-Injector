"""
font_to_arduino.py - Convert TTF fonts to Arduino 32x32 format
Usage: python font_to_arduino.py <font_file.ttf> [output_file.h] [characters]
"""

import sys
import os
from PIL import Image, ImageDraw, ImageFont
import numpy as np

RENDER_SIZE = 64  # font size used for rendering, before scaling down -- larger
                  # than the final 32px so small glyphs still have enough
                  # resolution to look clean after scaling

def render_ink(char, font, canvas_size=200):
    """Render a character anchored to a shared baseline and return its ink.

    Returns (char_img, top_offset_from_baseline, bottom_offset_from_baseline)
    where char_img is the cropped binary ink, or None if the glyph is blank
    (e.g. space). Offsets are in render-resolution pixels, relative to the
    shared baseline -- negative above the baseline, positive below (e.g. the
    tail of a 'y' or 'g').
    """
    origin_x = 4
    origin_y = canvas_size - 60
    img = Image.new('L', (canvas_size, canvas_size), 0)
    draw = ImageDraw.Draw(img)
    draw.text((origin_x, origin_y), char, font=font, fill=255, anchor='ls')

    binary = (np.array(img) > 128).astype(np.uint8)

    rows = np.any(binary, axis=1)
    cols = np.any(binary, axis=0)
    if not np.any(rows) or not np.any(cols):
        return None

    y_min, y_max = np.where(rows)[0][[0, -1]]
    x_min, x_max = np.where(cols)[0][[0, -1]]

    char_img = binary[y_min:y_max + 1, x_min:x_max + 1]
    top_offset = int(y_min) - origin_y
    bottom_offset = int(y_max) - origin_y

    return char_img, top_offset, bottom_offset

def place_scaled(char_img, top_offset, cell_scale, baseline_row, center_vertically=False):
    """Scale a glyph's cropped ink by the shared cell_scale and place it in
    a 32x32 grid. Baseline-anchored glyphs (letters, digits, '.', ',') are
    placed relative to the shared baseline_row, same as they'd sit in real
    text. Other symbols (+, =, %, brackets, etc.) don't have a natural
    baseline relationship to letters, so they're instead centered vertically
    in the cell when center_vertically is True."""
    h, w = char_img.shape
    new_h = max(1, round(h * cell_scale))
    new_w = max(1, round(w * cell_scale))

    char_pil = Image.fromarray((char_img * 255).astype(np.uint8))
    resized = char_pil.resize((new_w, new_h), Image.Resampling.LANCZOS)
    resized_array = (np.array(resized) > 128).astype(np.uint8)

    result = np.zeros((32, 32), dtype=np.uint8)

    if center_vertically:
        y_top = (32 - new_h) // 2
    else:
        y_top = baseline_row + round(top_offset * cell_scale)
    x_left = 16 - new_w // 2

    y0, x0 = max(0, y_top), max(0, x_left)
    y1, x1 = min(32, y_top + new_h), min(32, x_left + new_w)
    sy0, sx0 = y0 - y_top, x0 - x_left

    if y1 > y0 and x1 > x0:
        result[y0:y1, x0:x1] = resized_array[sy0:sy0 + (y1 - y0), sx0:sx0 + (x1 - x0)]

    return result.tolist()

def bitmap_to_arduino_array(bitmap):
    """Convert 32x32 bitmap to Arduino uint32_t array format"""
    rows = []
    for y in range(32):
        value = 0
        for x in range(32):
            if bitmap[y][x]:
                value |= (1 << (31 - x))  # MSB-left
        rows.append(f"0x{value:08X}")
    return rows

def get_char_name(char):
    """Get a descriptive name for the character"""
    if char == ' ':
        return "SPACE"
    elif char == '-':
        return "DASH"
    elif char == '.':
        return "DOT"
    elif char == ',':
        return "COMMA"
    elif char == '!':
        return "EXCLAMATION"
    elif char == '?':
        return "QUESTION"
    elif char == '\'':
        return "APOSTROPHE"
    elif char == '"':
        return "QUOTE"
    elif char == ':':
        return "COLON"
    elif char == ';':
        return "SEMICOLON"
    else:
        return f"'{char}'"

def main():
    if len(sys.argv) < 2:
        print("Usage: python font_to_arduino.py <font_file.ttf> [output_file.h] [characters] [scale]")
        print("\nExamples:")
        print("  python font_to_arduino.py Arial.ttf arial_font.h")
        print("  python font_to_arduino.py Arial.ttf arial_font.h \"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-!?.,\"")
        print("  python font_to_arduino.py Arial.ttf arial_font.h \"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-!?.,\" 1.15")
        print("\nscale (optional, default 1.0): overall size multiplier for every glyph.")
        print("  >1.0 makes glyphs bigger (may start clipping at the edges if pushed too far),")
        print("  <1.0 makes glyphs smaller with more padding around them.")
        sys.exit(1)
    
    font_path = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else "output_font.h"
    
    # Default character set (matches Digital-7 plus some extras)
    default_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-!?.,"
    chars = sys.argv[3] if len(sys.argv) > 3 else default_chars

    scale = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
    
    # Remove duplicates while preserving order
    seen = set()
    chars = ''.join([c for c in chars if not (c in seen or seen.add(c))])
    
    print(f"Converting {len(chars)} characters: {chars}")
    print(f"Scale: {scale}")
    
    font_name = os.path.splitext(os.path.basename(font_path))[0].upper()

    try:
        font = ImageFont.truetype(font_path, RENDER_SIZE)
    except Exception:
        font = ImageFont.load_default()
        print(f"Warning: Could not load {font_path}, using default font")

    # Derive ONE shared scale/baseline from the actual REQUESTED characters
    # (not the font's abstract ascent/descent metric, which reserves extra
    # headroom for accents and descenders that may not even be in your
    # character set, and would otherwise make letters render smaller than
    # they need to). Render every glyph once to measure its ink, then use
    # the tallest NON-descending glyph (one that doesn't dip below the
    # baseline, e.g. a capital letter or digit) as the reference height --
    # that's what should fill the available box, same as it would in the font
    # itself.
    ink_data = {}
    for char in chars:
        print(f"  Rendering '{char}'...")
        ink_data[char] = render_ink(char, font)

    DESCENDER_TOL = 2  # px at render resolution; ink dipping further than
                       # this below the baseline counts as a "descender"
    non_descender_heights = [
        img.shape[0] for (img, top, bottom) in ink_data.values()
        if img is not None and bottom <= DESCENDER_TOL
    ]
    if non_descender_heights:
        reference_height = max(non_descender_heights)
    else:
        all_heights = [img.shape[0] for (img, top, bottom) in ink_data.values() if img is not None]
        reference_height = max(all_heights) if all_heights else RENDER_SIZE // 2

    # Reserve room below the baseline for whatever actually dips there.
    # It's not just lowercase descenders (g/y/p/q/j) -- brackets, braces,
    # '%', '$', '!', '?' and ',' commonly extend a little below the
    # baseline by design too. Sizing purely off reference_height (as if
    # nothing dipped below it) is what clipped those off at the bottom of
    # the 32px grid.
    max_descent = max(
        (max(0, bottom) for (img, top, bottom) in ink_data.values() if img is not None),
        default=0
    )

    TOP_PAD = 2
    BOTTOM_PAD = 2
    available = 32 - TOP_PAD - BOTTOM_PAD  # total px span for reference_height + max_descent
    cell_scale = scale * available / (reference_height + max_descent)
    baseline_row = TOP_PAD + round(reference_height * cell_scale)

    char_map = {}
    for char in chars:
        data = ink_data[char]
        if data is None:
            char_map[char] = [[0] * 32 for _ in range(32)]
        else:
            img, top_offset, _ = data
            # Letters, digits, and '.'/','  sit on the baseline like they
            # would in real text. Everything else (+, =, %, brackets, etc.)
            # has no natural baseline relationship to letters, so it's
            # centered vertically in the cell instead.
            baseline_anchored = char.isalnum() or char in ('.', ',')
            char_map[char] = place_scaled(
                img, top_offset, cell_scale, baseline_row,
                center_vertically=not baseline_anchored
            )
    
    # Build a human-readable glyph order description for the header comment,
    # e.g. "'%', '*', '?', ',', DASH, '0'-'9', 'A'-'Z' (41 glyphs total)."
    glyph_order_desc = ", ".join(get_char_name(c) for c in chars)

    # Generate output file (single 2D array format, matching font.h style)
    with open(output_file, 'w') as f:
        f.write("#pragma once\n\n")
        f.write(f"// {font_name} style 32x32 bitmap font, converted from {os.path.basename(font_path)}.\n")
        f.write(f"// Glyph order: {glyph_order_desc} ({len(chars)} glyphs total).\n")
        f.write("// This order MUST match however characters are indexed into this\n")
        f.write("// array in your sketch -- if you add/remove/reorder glyphs here,\n")
        f.write("// update that mapping too.\n")
        f.write("// Lives in flash (PROGMEM) instead of RAM -- on ESP32 flash is\n")
        f.write("// memory-mapped, so it can still be indexed directly as font32x32[idx][row].\n\n")

        f.write("extern const uint32_t font32x32[][32] PROGMEM = {\n")

        for char in chars:
            bitmap = char_map[char]
            rows = bitmap_to_arduino_array(bitmap)

            f.write(f"// {get_char_name(char)}\n")
            f.write("{")
            for i, row in enumerate(rows):
                if i < 31:
                    f.write(f"{row},")
                else:
                    f.write(f"{row}")
            f.write("},\n")

        f.write("};\n")

    print(f"\nFont converted successfully to {output_file}")
    print(f"Total characters: {len(chars)}")
    print("\nTo use in your Arduino sketch:")
    print("1. Add this line to your sketch: #include \"" + os.path.basename(output_file) + "\"")
    print("2. Index into it directly, e.g. font32x32[idx] gives the 32 rows for glyph idx,")
    print("   where idx corresponds to the position of the character in the glyph order above.")

if __name__ == "__main__":
    main()