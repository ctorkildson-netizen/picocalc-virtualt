# Third-party material

This project's own code (`pico_main.c`, `vt_config.c`, `vt_config.h`, the
build files and the documentation) is under the BSD-2-Clause licence in
[`LICENSE`](LICENSE). Everything below came from elsewhere and keeps its own
licence.

## VirtualT — BSD, Stephen Hurd and Ken Pettit

<https://sourceforge.net/projects/virtualt/>. The emulation core in `core/`
(the 8085 CPU, memory, I/O, LCD and ROM tables) is VirtualT's, under the
notice in [`LICENSE-VirtualT.txt`](LICENSE-VirtualT.txt).

| Here | From VirtualT |
|---|---|
| `doins.c`, the `*rom.c` tables, `romstrings.c` and VirtualT's headers | VirtualT's own files, unchanged except `sound.h`, which declares `sound_click()` |
| `io.c` | VirtualT's, with one change: `SOUND 0` clicks, as on the real machine, where VirtualT played 500 Hz |
| `clock_vt.c` | VirtualT's `clock.cpp` clock-chip code, without its FLTK preferences |
| `mem_vt.c` | this project's slim replacement for VirtualT's `memory.c`, Model 100 and Tandy 200 only (`mem_m100.c` is an earlier Model 100-only version, not built) |
| `vt_glue.c`, `lcdtext.c`, `vt_basic.c`, `vt_export.c` | this project's, in place of VirtualT's FLTK front end; `vt_export.c` follows VirtualT's "Save to Host" |

The files marked as this project's are under [`LICENSE`](LICENSE) like the
rest of its code.

No ROM images are included. The `core/*rom.c` files are VirtualT's tables of
ROM addresses, not ROM contents.

## picocalc-text-starter — MIT, Blair Leduc

<https://github.com/BlairLeduc/picocalc-text-starter>, a submodule in
`modules/picocalc-text-starter`, used unchanged. The build compiles its
`drivers/` for the LCD, fonts, SD card and FAT32, audio, and the keyboard and
battery controller (`southbridge.c`).

    MIT License

    Copyright (c) 2025 Blair Leduc

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## picocalc-shell — `picocalc_shell.h` only, MIT, Blair Leduc

<https://github.com/BlairLeduc/picocalc-shell>, a submodule in
`modules/picocalc-shell`. Only its header `include/picocalc_shell.h` is
used, so VirtualT can hand control back to the shell. The shell itself is
GPL-3.0, but that header is also offered under the MIT licence so that
programs under any licence can include it. Nothing else from the shell is
compiled in.

## Raspberry Pi Pico SDK and its libraries

<https://github.com/raspberrypi/pico-sdk>, BSD-3-Clause, Raspberry Pi Ltd.
The WiFi time sync links the SDK's copies of lwIP (BSD-3-Clause, the lwIP
authors) and the CYW43 WiFi driver and firmware, which carry their own
licence from Raspberry Pi Ltd allowing use on Raspberry Pi microcontrollers.

## The programs in `programs/`

Most of the 55 programs come from the CompuServe M100SIG library of the 1980s,
kept today at <https://www.web8201.net/>, with Club 100
(<https://www.club100.org/>). Each is its author's work; the authors' names
and comments are left as written. PROGRAMS.TXT lists every author.

- Where we fixed a program, a line starting `2026:` near its top says what.
- `ROG.BA` is Willard Goosey's, 1998-2016, under the GPL, as its first line
  says.
- `HPCALC.BA` is Scott Schad's shareware; its notice is in `HPCALC.DOC`.
- `LIFE.BA` and `README.DO` were written for this project.
- Two programs whose authors asked that they not be redistributed
  (CLOCK.PMS and NAV2) are left out; PROGRAMS.TXT says where to find them.

If you wrote one of these and would rather it weren't included, open an issue
and it will be removed.
