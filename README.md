# dcjson

A very fast terminal JSON viewer for Linux, written in pure C17 with no
dependencies. It opens large files instantly, lets you expand and collapse
the tree, search it, jump to paths, and copy values, keys and paths to the
clipboard. Copying works through SSH too (OSC 52). Colours come from seven
themes. It is built to be used over SSH: it redraws only what changed and
lets the terminal scroll, so moving around sends very little data.

```
─ dcjson ─ users.json ─ 48.2 MiB ─ 1,204,331 values ────────────────── 12% ─
▾ {                                                                  5 keys
│   "version": 3,
│ ▾ "users": [                                                12,000 items
│ │ ▾ 0: {                                                           6 keys
│ │ │   "id": 1001,
│ │ │   "name": "Matti Meikäläinen",
│ │ │   "email": "matti@example.com",
│ │ │   "active": true,
│ │ │   "manager": null,
│ │ │ ▸ "tags": ["admin", "dev", "ops"]                             3 items
│ │   },
│ │ ▸ 1: {"id": 1002, "name": "Liisa Virtanen", "email": "liisa@ex…  6 keys
│ │ ▸ 2: {"id": 1003, "name": "Pekka Korhonen", "email": "pekka@e…   6 keys
 .users[0].email                           / search  y copy  ? help  q quit
```

* **No dependencies:** only libc and system calls. No ncurses, no locale
  database, no helper processes.
* **Fast:** about 500 MiB/s on typical API dumps and 2 GiB/s on
  string-heavy data (one core of a desktop CPU, see
  [Measured results](#measured-results)). Big files open at once: the first
  screen appears after about 30 ms while the rest is parsed in the
  background.
* **Small:** about 98 KiB stripped (x86-64, gcc 15). Its only shared library
  is libc.
* **Light on SSH:** moving the cursor sends about 400 bytes, and scrolling
  one line about the same. A full 120×40 screen is about 4 KiB.
* **Safe with untrusted input:** bytes from the file never reach the
  terminal as control sequences.
* **Single-threaded event loop.** When nothing happens, nothing runs.

## Build and install

Requirements: Linux, GCC (or Clang), GNU make.

```bash
make                 # release build -> ./dcjson  (-std=c17 -O2 -flto -s)
make debug           # ./dcjson-debug with -O0 -g3, ASan + UBSan, all warnings
make test            # builds build/tests (sanitized) and runs the whole suite
make bench           # parse speed and terminal output (tests/bench.sh)
make clean
sudo make install    # /usr/local/bin/dcjson   (PREFIX=... / DESTDIR=... supported)
```

On Windows, build and run it in WSL.

## Usage

```bash
dcjson data.json                    # open a file (memory-mapped, not copied)
curl -s https://api.example/x | dcjson   # standard input; keys come from /dev/tty
dcjson events.jsonl                 # JSON Lines: automatic for .jsonl / .ndjson
dcjson -l concatenated.json         # several top-level values in any file
dcjson --expand 1 big.json          # start with only the top level expanded
dcjson --lenient config.json        # allow // and /* */ comments, trailing commas
dcjson --theme nord --no-mouse data.json
```

Without the viewer (scripts, pipes):

```bash
dcjson --check data.json            # validate; prints FILE:LINE:COL: message
dcjson --print data.json            # pretty-print (2 spaces; --indent N)
dcjson --minify data.json           # no white space
dcjson --path '.users[3].name' data.json        # one value, pretty-printed
dcjson --raw --path '.users[3].name' data.json  # strings without quotes
dcjson data.json | less             # stdout is not a terminal: pretty-prints
```

Exit status: 0 success, 1 invalid JSON or a path that is not there, 2
usage or I/O error. `dcjson --help` lists every option.

### Keys

| Key | Action |
|---|---|
| `j` `k` `↓` `↑` | next / previous line |
| `J` `K` | next / previous sibling |
| `p` `P` | parent / top-level value |
| `g` `G` `Home` `End` | first / last line |
| `PgDn` `PgUp` `f` `b` | page down / up |
| `Ctrl-d` `Ctrl-u` | half a page down / up |
| `Ctrl-e` `Ctrl-y` | scroll one line without moving the cursor |
| `zz` `zt` `zb` | cursor line to the middle / top / bottom of the screen |
| `<` `>` | scroll sideways |
| `l` `→` | expand; on an expanded value, go to its first child |
| `h` `←` | collapse; on a collapsed value, go to its parent |
| `Space` `Tab` | toggle |
| `L` `H` | expand / collapse the whole subtree |
| `1` … `9` | expand everything down to that level |
| `0` `*` | collapse / expand everything |
| `/` | search keys and values (smart case: lower case ignores case) |
| `n` `N` | next / previous match |
| `:` | go to a path: `.users[3].name`, `users[-1]`, `."odd key"`, `#2` (third value) |
| `e` | go to the parse error |
| `Enter` `v` | show the value in full, wrapped (long strings, big objects) |
| `yy` `yp` `ym` | copy the value: as in the source / pretty-printed / minified |
| `yr` | copy a string without quotes and escapes |
| `yk` | copy the key |
| `y.` `yj` | copy the path, jq style (`.users[0]["odd key"]`) / JavaScript style |
| `c` | closing brackets on lines of their own (saved) |
| `\|` `i` | indentation guides / array indices |
| `s` `S` | next / previous colour theme (saved) |
| `Esc` | clear the search highlight, cancel a prefix, prompt or search |
| `Ctrl-l` | redraw |
| `?` `F1` | help |
| `q` `Ctrl-c` | quit |

Mouse: a click selects a line, a click on `▸` / `▾` or a double click
toggles, and the wheel scrolls. `--no-mouse` gives the mouse back to the
terminal, for its own text selection. Many terminals also let you select
with Shift held down while an application uses the mouse.

### What is shown

* Each value is on its own line, with its key (objects) or its index
  (arrays, `i` hides them). Colours: keys, strings, escape sequences inside
  strings, numbers, booleans, `null` and punctuation each have their own
  colour.
* Strings and numbers are shown **exactly as they are in the file**: escapes
  stay escapes (`"ä"`, `"\n"`), and numbers keep their spelling and
  precision (no rounding through a double). `Enter` shows a string decoded,
  wrapped to the screen width.
* A collapsed object or array is shown as a one-line preview
  (`{"id": 1002, "name": "Liisa", …}`), with the number of keys or items on
  the right.
* Closing brackets get lines of their own, as in the file. `c` turns that
  off for a denser tree. It is saved, and so is `--closers on|off`.
* Lines too long for the screen end in `…`. `<` `>` scroll sideways, and
  `Enter` shows the whole value.
* The bottom line shows the path of the cursor in jq syntax. The top line
  shows the file, its size, the number of values and the position. While
  parsing it shows the progress, and after a parse error it shows the
  error position.

### Large files and slow pipes

A file is memory-mapped, not read into memory. The parser runs in the
event loop in slices of 8 ms. The first screen is drawn as soon as it is
filled (or after 30 ms), and the rest is parsed while you already look
and move around. Containers that are still being read show `12,000+ items`.
`G` goes to the end of what has been parsed so far. Searches wait for the
parser when they get ahead of it. Input from a pipe is shown as it
arrives, so `curl … | dcjson` displays the start of a slow response
straight away.

Inputs under 64 KiB start fully expanded, bigger ones with two levels
(`--expand N` or `expand = N` in the configuration file change this).

When the input is not valid JSON, everything before the error stays
viewable. The title line shows `parse error at LINE:COL`, and `e` jumps to
the place. Containers that were never closed are marked `(cut short)`.

### Copying over SSH

dcjson copies with **OSC 52**, an escape sequence that asks *your local
terminal* to put text on the clipboard. It works on a remote machine
through SSH, with no X forwarding and no clipboard tool on the server.

| Terminal | OSC 52 copy |
|---|---|
| kitty, WezTerm, foot, Alacritty, Ghostty, Windows Terminal | works by default |
| iTerm2 | enable *Applications in terminal may access clipboard* |
| xterm | allow `SetSelection`: `XTerm*disallowedWindowOps: 20,21,SetXprop` |
| GNOME Terminal and other VTE terminals | not supported: use `--clipboard command` or `file` |
| tmux | `set -g set-clipboard on` (or `allow-passthrough on`) in `~/.tmux.conf` |
| GNU screen | wrapped automatically (when `$STY` is set) |

Inside tmux dcjson sends the sequence both plainly and wrapped in tmux's
passthrough, so either tmux setting works. Copies larger than `clip-max`
(default 1 MiB) are written to a file instead, because some terminals drop
very long sequences. The status line names the file.

Other ways to copy (`--clipboard MODE` or `clipboard = MODE`):

| Mode | Where the copy goes |
|---|---|
| `auto` | OSC 52, wrapped for tmux or screen when running inside them (default) |
| `osc52`, `tmux`, `screen` | OSC 52, plain or wrapped for that multiplexer |
| `command` | the standard input of `copy-command`, e.g. `wl-copy`, `xclip -selection clipboard`, `pbcopy` |
| `file` | `$XDG_RUNTIME_DIR/dcjson-clipboard-UID.json` (or `$TMPDIR`, `/tmp`) |

A terminal does not report whether it accepted OSC 52. When nothing
arrives on your clipboard, check the table above.

### Configuration file

dcjson reads `$XDG_CONFIG_HOME/dcjson/config`, by default
`~/.config/dcjson/config`, if it exists. Each line is one option, written as
its long name without the dashes. `#` starts a comment:

```ini
# ~/.config/dcjson/config
theme = gruvbox          # written by the s key
closers = off            # written by the c key
expand = 3
guides = on
indices = on
clipboard = auto
copy-command = wl-copy   # used with clipboard = command
clip-max = 4M
no-mouse
lenient
indent = 4
max-depth = 20000
```

* An option given on the command line wins over the file.
* Errors name the file and the line, for example
  `dcjson: /home/me/.config/dcjson/config:3: unknown option 'colour'`.
* `s` and `c` change only their own line (or add it). Comments and the other
  lines stay as they are, and the file is replaced atomically. With
  `--no-config` nothing is read or written. `--config FILE` uses another file.

### Themes

| Theme | Look |
|---|---|
| `default` | blue keys, green strings, yellow numbers on a dark background |
| `nord` | cool blue and grey ([Nord](https://www.nordtheme.com/) palette) |
| `gruvbox` | warm, retro ([gruvbox](https://github.com/morhetz/gruvbox) dark) |
| `dracula` | [Dracula](https://draculatheme.com/) |
| `solarized` | [Solarized](https://ethanschoonover.com/solarized/) dark |
| `mono` | grey only, for minimal distraction |
| `light` | for terminals with a light background |

The colour depth is detected from `COLORTERM` and `TERM`: 24-bit, 256 or
16 colours. With 16 colours the themes use the terminal's own palette.
`NO_COLOR`, `TERM=dumb` and `--no-colors` turn colours off. The cursor line
is then shown in reverse video and search matches are underlined. `--ascii`
draws `v`, `>`, `|` and `~` instead of `▾`, `▸`, `│` and `…`, and is chosen
automatically when the locale is not UTF-8.

## Performance design

* **One pass, no copies.** The parser turns every JSON value into a 32-byte
  entry of one flat table, in document order. It records where the value is
  in the input, how far its subtree reaches, its parent, the number of
  children and where its key is. Strings are not decoded and numbers are
  not converted: the screen shows the input bytes. Decoding happens only
  when something asks for it (copying `yr`, the full-value view).
* **SWAR string scanning.** String contents are scanned eight bytes at a
  time: one 64-bit load and a few bit operations find the next quote,
  backslash or control byte, and tell whether the string is pure ASCII.
* **The node table** is an anonymous mapping that grows with `mremap()`
  (no copying) and uses transparent huge pages, so a table of hundreds of
  MiB costs a few hundred page faults instead of a hundred thousand. The
  input file is memory-mapped with `MADV_SEQUENTIAL` while parsing and
  `MADV_RANDOM` afterwards.
* **Resumable parser.** The parser is a state machine with its own stack,
  not recursion. It can stop at any token boundary and continue, also in
  the middle of a string that is still arriving from a pipe. Deep nesting
  cannot overflow the C stack (the limit is `--max-depth`, default 10000).
* **No list of visible lines.** A line is a table index, plus a flag for
  closing-bracket lines. Expanding and collapsing flip one bit. The next
  line after a collapsed subtree is found in O(1) through the "end of
  subtree" index, and the line before it in O(depth) through the parent
  chain. So expanding the root of a 10-million-value array costs nothing,
  and `G` is instant.
* **Diff drawing** (from dcmon). Every screen line is built in a buffer,
  hashed with FNV-1a and sent only when it changed. Colour codes are sent
  only when the colour changes, and only the foreground part when only the
  foreground changes. Runs of spaces become "erase to end of line" plus a
  cursor move.
* **Terminal scrolling.** When the view moves by a few lines, dcjson sets a
  scroll region over the tree area and sends one *scroll up* / *scroll down*
  sequence. The terminal moves the text it already has, and only the
  uncovered lines are drawn.
* **Key coalescing.** All keys that are waiting are handled before one frame
  is drawn, so a held-down key over a slow link does not queue up hundreds
  of frames.
* **Search** runs on the raw input with `memmem()` (case-sensitive) or a
  two-case `memchr()` scan (smart case), in slices between key checks. The
  match is mapped to its value by a binary search in the table.
* **Sibling indices** (`[12345]` in front of array elements, and in paths)
  are cached per depth, so walking a long array line by line stays O(1)
  per line.

### Safety

The input is untrusted. Raw control characters (possible with `--lenient`),
C1 control codes, bidirectional overrides and invalid UTF-8 are drawn as
`�`. Escapes such as `\u001b` are shown as text. The test suite feeds hostile
strings through the renderer into a VT emulator that fails on any control
sequence the renderer did not mean to send.

## Measured results

`make bench` on an Intel Core i5-10400F (2.9 GHz), WSL2, gcc 15.2, warm page
cache, best of three runs of `dcjson --check --stats`:

| Input (≈100 MiB) | Time | Throughput | Values | Peak RSS |
|---|---|---|---|---|
| records: array of user objects, pretty-printed | 0.20 s | 505 MiB/s | 5.7 M | 275 MiB |
| numbers: arrays of numbers (84.6 MiB) | 0.27 s | 317 MiB/s | 12.8 M | 478 MiB |
| strings: long strings with escapes and UTF-8 | 0.05 s | 1.84 GiB/s | 73 k | 105 MiB |
| deep: nested to 40 levels, compact | 0.55 s | 183 MiB/s | 19.7 M | 701 MiB |
| lines: JSON Lines records | 0.22 s | 458 MiB/s | 6.9 M | 313 MiB |

Memory is the mapped file (page cache, shared, not copied) plus 32 bytes per
value. Time is roughly 25 ns per value plus 0.5 ns per byte.

Bytes sent to the terminal (120×40, 256 colours, records file, measured with
`build/tests --bench-render`):

| Action | Bytes |
|---|---|
| full screen | 4.1 KiB |
| cursor one line (on screen) | 389 |
| scroll one line (cursor at the bottom) | 400 |
| page down | 3.7 KiB |

The stripped release binary is 98 KiB (`-O2`). `make OPT=-Os` gives 79 KiB
and parses value-dense input somewhat more slowly.

## Tests

`make test` builds the suite with AddressSanitizer, UndefinedBehaviorSanitizer
and leak detection, and runs it:

* **Parser:** accepted and rejected inputs (numbers, escapes, literals,
  BOM, depth limit, lenient comments and trailing commas), the node table,
  error positions, decoding with surrogate pairs.
* **Resumable parsing:** fixed and 3000 random (and randomly corrupted)
  documents are parsed in pieces of 1 to 16 bytes with tiny budgets. The
  node table and error must equal a one-shot parse, byte for byte.
* **Navigation:** for random documents with random expanded states, with
  and without closing-bracket lines, walking `next` must equal a naive
  recursive walk. `prev` must invert `next`, the ordering must agree, and
  `view_fix` must always give a visible line. A document still being parsed
  is checked too.
* **Paths and printers:** jq and JavaScript paths round-trip for every
  value. Pretty and minified output of random documents re-parses to the
  same values, and documents cut short print closed.
* **Search** (against a naive search), **UTF-8** and widths, **key and
  mouse decoding**, **OSC 52** bytes (base64 lengths, tmux and screen
  wrapping), **configuration** reading, saving and merging.
* **Renderer:** frames go through a small VT emulator. Lines are checked,
  and 120 random scroll / move / toggle steps must give exactly the screen
  of a full repaint, which checks the scroll-region optimisation. One step
  down must draw at most four lines. Hostile strings must not produce any
  unintended control sequence.
* **End to end:** `--check`, `--print`, `--minify`, `--path`, `--raw`,
  `--lines`, `--lenient` and exit codes of the debug binary.

## Project layout

```
src/main.c      input, event loop, keys, search and parse scheduling, print modes
src/json.c/h    resumable parser and the value table
src/view.c/h    visible lines: next / previous, expand / collapse
src/render.c/h  drawing: line diffs, scroll regions, colours, help and text views
src/fmt.c/h     writing values: source, pretty, minified, decoded
src/clip.c/h    clipboard: OSC 52 (tmux, screen), command, file
src/path.c/h    jq / JavaScript paths: building and resolving
src/search.c/h  substring search with smart case
src/input.c/h   key, mouse and paste decoding
src/utf8.c/h    UTF-8 and terminal cell widths
src/cli.c/h     options and the configuration file
src/theme.c/h   colour themes
src/terminal.c/h raw mode, alternate screen, output buffer (from dcmon)
src/util.c/h    small helpers
tests/test.c    test suite;  tests/gen.c  test data;  tests/bench.sh  benchmark
DESIGN.md       design notes
```

## License

MIT, see [LICENSE](LICENSE).
