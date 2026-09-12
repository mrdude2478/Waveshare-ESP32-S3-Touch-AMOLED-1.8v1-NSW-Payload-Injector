# mod2header

Converts a binary file (e.g. a ProTracker `.mod` file) into a `.h` header
containing a `PROGMEM` byte array, in the exact format your ESP32 sketches
expect:

```c
extern const uint8_t song[] PROGMEM  = {
  0x70, 0x6f, 0x70, ...
};

extern const size_t song_size = sizeof(song);
```

(Your `song.h` array is just the raw bytes of a standard Amiga ProTracker
module — it starts with the module title `"popcorn"` and 22-byte sample
name fields — dumped as hex. This tool reproduces that dump exactly,
including line endings, spacing, and the trailing entries.)

## Build (Windows, with cl.exe)

1. Open a **Developer Command Prompt for VS** (or run `vcvarsall.bat x64`
   in a normal cmd window) so that `cl.exe` is on your PATH.
2. In that prompt, `cd` to this folder and run:

   ```
   build.bat
   ```

   or directly:

   ```
   cl /EHsc /std:c++17 /O2 mod2header.cpp /Fe:mod2header.exe
   ```

This produces `mod2header.exe`.

## Usage

**Drag and drop:** Drag a `.mod` file onto `mod2header.exe`. It will write
`song.h` (with array name `song`) into the same folder as the `.mod` file —
matching your existing workflow.

If you drag multiple `.mod` files onto it at once, it instead writes one
`<name>.h` per file, with the array named after each file (since `song` for
everything wouldn't make sense in that case).

**Command line:**

```
mod2header.exe path\to\song.mod
mod2header.exe path\to\song.mod --name=my_array
mod2header.exe path\to\song.mod --name=my_array --out=C:\path\to\out.h
```

- `--name=` overrides the array name (default: `song`).
- `--out=` overrides the output file path (default: `<name>.h` next to the
  input file).

The program pauses with "Press Enter to exit..." at the end so the console
window doesn't vanish immediately when run via drag-and-drop.

## Verified

The output has been checked byte-for-byte identical against a reference
`song.h` (regenerating the original `.mod` from it and re-running this tool
reproduces the exact same file, including its CRLF line endings).
