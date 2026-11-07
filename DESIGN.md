# dcjson – design

dcjson is a terminal JSON viewer in C17 for Linux. It follows the dcmon model:
no dependencies (no ncurses), a single-threaded `poll()` loop, diff-based
drawing with per-line hashes, colour themes for 16 / 256 / 24-bit
terminals, and the same Makefile layout (release, sanitized debug, `make
test`, `make bench`). This document records the decisions behind the code.
User-facing behaviour is described in [README.md](README.md).

## 1. Goals

| Goal | Target | Measured (i5-10400F) |
|---|---|---|
| Fast open | first screen in ≈30 ms for any size; 100 MiB fully parsed < 0.4 s | 0.05–0.55 s per 100 MiB, depending on value density |
| Small memory | file mapped (page cache, not copied) + 32 B per value | as designed |
| Small binary | ≈100 KiB stripped, libc only | 98 KiB (`-O2`), 79 KiB (`-Os`) |
| Little SSH traffic | one-line scroll ≈ one new line | ≈400 B per step at 120×40 |
| Idle when idle | no timers without a reason | `poll(-1)` when nothing is pending |
| Safe | input bytes never act as terminal controls | tested with a VT emulator |

Decisions: Linux only; closing brackets on their own lines by default,
switchable (`c`, `--closers`, `closers =`); background parsing so the start
of a file is visible at once; name `dcjson`, MIT licence.

Not in scope: editing JSON, a jq-like query language, other platforms.

## 2. Architecture

```
 file / stdin
      │ mmap (regular files) or a growing read buffer (pipes)
      ▼
 json.c    resumable one-pass parser → flat value table (no copies)
      ▼
 view.c    visible lines = table index + closer flag; JF_OPEN bit per container
      ▼
 render.c  line buffer → FNV-1a hash → send changed lines only;
           scroll regions (CSI S / T) when the view moves
      ▼
 terminal.c (from dcmon)  raw mode, alternate screen, 64 KiB output buffer
```

Side modules: `fmt.c` (value writers on a streaming sink), `clip.c` (OSC 52,
command, file), `path.c` (jq / JS paths), `search.c`, `input.c` (keys, SGR
mouse, bracketed paste), `utf8.c` (decoding and cell widths), `cli.c`
(options and the config file, from dcmon), `theme.c` (from dcmon, new colour
slots).

## 3. The value table

```c
typedef struct {
    uint64_t off;    /* first byte of the value                         */
    uint32_t len;    /* scalar: length; container: closing bracket - off */
    uint32_t end;    /* index one past the subtree (JN_PENDING if open)  */
    uint32_t parent; /* JN_NONE for top-level values                     */
    uint32_t n;      /* number of children                               */
    uint32_t kd;     /* off - offset of the key's opening quote; 0 = none */
    uint16_t depth;
    uint8_t  type, flags; /* JF_OPEN, JF_ESC, JF_NONASCII, JF_CUT        */
} jnode_t;           /* 32 bytes */
```

* **Pre-order, flat.** Children follow their parent, so a subtree is the
  index range `[i, end)`. Skipping a subtree is O(1), and "last node of the
  subtree" is `end - 1`.
* **Nothing decoded.** Strings keep their escapes and numbers keep their
  text, so nothing is lost (big integers, exact decimals). Decoding happens
  only on demand (`yr`, `yk`, the full-value view, `--raw`).
* **Lengths that do not fit** in 32 bits (values over 4 GiB) are stored as
  `JLEN_BIG` and recomputed by scanning when needed.
* **Limits:** 2³² − 3 values, 65535 levels (default limit 10000), 64-bit
  offsets.
* **Storage:** an anonymous `mmap` (`MAP_NORESERVE`), grown by `mremap`
  without copying, with `MADV_HUGEPAGE` above 4 MiB. The first size is
  estimated as one value per 16 input bytes. This cut the system time of a
  20-million-value parse from 0.22 s to 0.06 s.

## 4. The parser

An explicit state machine (`S_TOP, S_VALUE, S_ARR_FIRST, S_ARR_NEXT,
S_OBJ_FIRST, S_KEY, S_COLON, S_AFTER`) with its own container stack.
`json_parse(d, budget)` returns after about `budget` bytes, at a token
boundary.

* **Resumable.** If a token is cut off by the end of the available input
  (a pipe), the parser stops in front of it and sets `waiting`, and the
  token is scanned again when more data arrives. Strings are the exception:
  their scan position and flags are saved, so a large string arriving
  slowly is not rescanned from the start each time.
* **SWAR strings.** Eight bytes per step: zero-byte tricks find `"`, `\` and
  bytes below 0x20 in one 64-bit word, and the high bits give the
  non-ASCII flag. Little-endian only, with a byte loop as fallback.
* **Strict RFC 8259** by default: number grammar (no `01`, `1.`, `.5`),
  escape validation including `\uXXXX` hex, no raw control characters,
  one top-level value unless `--lines`. UTF-8 is not validated while
  parsing. It is checked when drawing (invalid bytes show as `�`).
* **`--lenient`:** `//` and `/* */` comments, trailing commas, raw control
  characters in strings.
* **Errors** stop parsing but keep the table: open containers are closed
  where they stand and marked `JF_CUT`. The viewer shows everything before
  the error, the line:column (in code points) and `e` jumps there.
* **Verification:** the test suite parses fixed and 3000 random (partly
  corrupted) documents in 1–16-byte pieces with 1–8-byte budgets and
  requires the same table and error as a one-shot parse.

### 4.1 Background parsing

No threads. The event loop runs the parser in slices of at most 8 ms (the
clock is checked every 1 MiB) and checks the keyboard between slices.

* Before the first frame the parser runs until it is done or 30 ms have
  passed. Inputs under 4 MiB are parsed completely first, so small files
  get one frame without flicker.
* Everything already parsed is final (pre-order). Only containers on the
  parser stack have `end == JN_PENDING`. Navigation stops at the parsed
  end, and counts show `12,000+ items`.
* Progress is redrawn at most four times a second. A frame is drawn right
  away when the screen is not yet full, so a slow pipe shows data as it
  comes.
* `G` goes to the end of what has been parsed so far (with a message).
  Search waits for the parser when it gets ahead of it. Copying a container
  that is still open is refused with a message.
* Pipes: `poll()` reports data, at most a few 1 MiB `read()`s are done per
  turn, and the buffer grows by doubling (`realloc`, which `mremap`s large
  blocks). The keyboard is then read from `/dev/tty`.
* Inputs under 64 KiB that finish parsing within a second start fully
  expanded. Others start with `expand` levels (default 2).

## 5. Lines, expanding and navigation (view.c)

A line is `row_t = index << 1 | closer`. There is no list of visible lines:
the `JF_OPEN` bits decide visibility.

* **next(i):** if i is expanded, the first child `i + 1`. Otherwise the line
  after the subtree: if i is the last child of a finished parent p, the
  next line is p's closing line (or, without closer lines, the line after
  p). Otherwise it is node `end(i)`. O(1), or O(depth) at the end of
  containers.
* **prev(i):** if the parent is `i - 1`, the parent. Otherwise the previous
  sibling s (the ancestor of `i - 1` under the same parent), and from it the
  *last visible line* of its subtree. That line is found by walking down
  the path from s to `end(s) - 1`. O(depth).
* **Order** (for keeping the cursor on screen): node lines by `2·i`, closing
  lines by `2·end − 1`, with inner closing lines first.
* **fix(r):** the nearest visible line: the highest collapsed ancestor.
  Used after collapsing, after toggling closer lines, and for `top`.
* **Operations:** toggle is one bit. Subtree expand / collapse and "expand to
  level N" are linear scans over the range, which is about 10 ms for 10
  million values. Search results and path jumps expand the ancestors.

## 6. Drawing (render.c)

The dcmon scheme is kept: each line is built in a buffer, hashed with
FNV-1a, and sent only when its hash changed. Consecutive changed lines are
joined with CR LF, others are positioned with CUP. Additions for a viewer:

1. **Scroll regions.** If the new first line was the k-th line of the last
   frame (or the reverse), the renderer sends `CSI 2;H-1 r`, `CSI k S` or
   `T`, `CSI r`, and shifts its hash array by k. Only the uncovered lines
   (and the old and new cursor lines) are drawn. A test compares 120 random
   incremental frames with full repaints in a VT emulator.
2. **Short colour codes.** The renderer tracks the current colour slot and
   sends only the foreground (`ESC[38;5;Nm`) when boldness and background
   stay the same, and a full `ESC[0;…m` otherwise. Every line ends in the
   default colour, so it can be sent on its own.
3. **Gaps** before the right-aligned counts and at line ends are "erase to
   end of line" plus a cursor move, not spaces. The cursor line relies on
   background colour erase in 256 and 24-bit colour modes. With 16 colours
   or none, where the highlight may be reverse video, it uses spaces.
4. **Previews.** A collapsed container is drawn on one line from its
   children (keys, scalars, nested brackets) until the width runs out. The
   line builder replaces the last cell with `…` when something does not fit.
5. **Wide characters** use a small built-in width table (East Asian Wide,
   emoji, combining marks), independent of the server's locale. A wide
   character cut by the left edge becomes spaces.
6. **Safety.** Only bytes 0x20–0x7E and valid, printable UTF-8 reach the
   terminal. C0 / C1 controls, DEL, bidirectional controls and invalid
   sequences are drawn as `�` (`?` in ASCII mode).

Measured at 120×40, 256 colours: full screen 4.1 KiB, cursor move 389 B,
one-line scroll 400 B, page down 3.7 KiB.

## 7. Colours

Theme engine from dcmon (default, nord, gruvbox, dracula, solarized, mono,
light; RGB + 256 index + 16-colour variant). Slots: text, dim, border,
title, key, string, escape (inside strings), number, boolean, null,
punctuation, info (counts, indices), guide, match, alert, good. The cursor
line uses each theme's cursor background. Without colours it is reverse
video, and matches are underlined. `s` / `S` cycle the themes and save the
choice.

## 8. Clipboard

* **OSC 52** (`ESC ] 52 ; c ; base64 BEL`) is the only way to reach the local
  clipboard from a remote shell without X forwarding. Base64 is encoded in
  4 KiB blocks straight into the output buffer, so no full-size copy is
  made.
* **tmux:** in `auto` mode the sequence is sent plainly (for
  `set-clipboard on`) and wrapped as `ESC Ptmux; ESC ESC ]52…` (for
  `allow-passthrough on`). **screen:** DCS-wrapped in 768-byte pieces.
* **Size limit:** every copy is generated twice, first into a counting sink.
  Copies over `clip-max` (default 1 MiB) go to
  `$XDG_RUNTIME_DIR/dcjson-clipboard-UID.json` (mode 0600, `O_NOFOLLOW`)
  instead.
* **`command` mode** forks `sh -c copy-command` with the data on stdin and
  stdout / stderr on `/dev/null`. **`file` mode** always writes the file.
* **Formats:** `yy` source text, `yp` pretty, `ym` minified, `yr` decoded
  string, `yk` decoded key, `y.` jq path, `yj` JavaScript path. The writers
  in `fmt.c` copy scalars and keys verbatim from the source, so `yp` and
  `ym` never change a value.

## 9. Search and paths

* **Search** runs on the raw input: `memmem()` when the query has upper-case
  letters, otherwise an ASCII case-insensitive scan driven by two `memchr()`
  streams (one per letter case). It runs as a job of 32 MiB chunks between
  key checks, so `Esc` cancels it. A match is mapped to its value by a
  binary search over `off`; a match inside a key belongs to that key's
  value. Matches at or before the cursor's value are skipped. Search wraps
  once, and backward search scans blocks from the end. Visible matches are
  highlighted.
* **Paths:** built from the parent chain. Keys that are identifiers are
  written `.key`, others `["key"]` (escapes kept). The index of an element
  in its parent is cached per depth, so line-by-line movement in a long
  array stays O(1). The resolver accepts jq and JavaScript forms, quoted
  keys, negative indices and `#N` for the N-th top-level value, and
  compares keys decoded.

## 10. Event loop and terminal

One `poll()` over a `signalfd` (INT TERM HUP QUIT WINCH TSTP CONT), the
keyboard and, while reading a pipe, the input. The timeout is 0 while
parsing or searching, the remaining time of a status message, 25 ms after a
lone `ESC` (to tell Esc from a key sequence), 250 ms for progress while a
pipe is quiet, and infinite otherwise. All waiting keys are decoded and
handled before one frame is drawn. Ctrl-Z restores the terminal, stops, and
redraws after `SIGCONT`. A failed write (the SSH session is gone) ends the
program. Crash signals restore the terminal (dcmon's handler). The mouse
uses SGR mode 1006 and paste uses bracketed paste: pasted text goes into a
prompt and is ignored elsewhere, so it is never run as commands.

## 11. Tests and measurement

`tests/test.c` (ASan + UBSan + leak detection) covers the parser,
resumable parsing, navigation against a naive walk, paths and printers
(round trips), search against a naive search, UTF-8, key decoding, OSC 52
bytes, the configuration file, the renderer through a VT emulator
(including the scroll-region equality test and hostile input), and end-to-end
runs of the binary. `tests/gen.c` generates records / numbers / strings /
deep / lines inputs, and `tests/bench.sh` reports parse speed, peak RSS and
bytes per key.

## 12. Possible later work

* An SSE2 / NEON structural scanner if parsing needs to be faster (deep,
  value-dense input is the slow case: ≈25 ns per value).
* A 24-byte node (merging `kd` and `n`) to cut memory by a quarter.
* Following a growing file (`tail -f` for JSON Lines).
