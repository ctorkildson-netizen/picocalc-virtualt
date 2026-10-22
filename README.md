# VirtualT for PicoCalc

A TRS-80 Model 100 and Tandy 200 emulator for the ClockworkPi PicoCalc
(RP2350: Pico 2 or Pico 2 W), built on the emulation core of
[VirtualT](https://virtualt.sourceforge.net) by Ken Pettit and contributors.

The TRS-80 Model 100 (Radio Shack, 1983) was one of the first notebook
computers: a full keyboard, an 8-line 40-column LCD and 20 hours on four AA
batteries, with BASIC, a text editor, a terminal program, an address book and
a scheduler built in. A large hobby library of programs grew up around it,
much of it still online. The Tandy 200 (1985) is its flip-top sibling with a
16-line screen and Multiplan, Microsoft's spreadsheet before Excel.

This port runs the real Model 100 ROM at the real machine's speed, with the
PicoCalc's screen, keyboard, sound and SD card standing in for the Model 100's.
It comes with 55 tested Model 100 and Tandy 200 programs.

**Status:** alpha. A hobby project, fixed when time allows; bug reports are
welcome as [issues](../../issues) -- see *REPORTING A PROBLEM* in
[VIRTUALT.TXT](VIRTUALT.TXT) for what to include.

## Installing

Download the release zip and follow *INSTALL* in [VIRTUALT.TXT](VIRTUALT.TXT).
In short: `virtualt.uf2` goes in `pico2-apps/` on the SD card; a `virtualt/`
folder holds your own Model 100 ROM image (not included -- it is Microsoft's
copyright) and the `m100` and `m200` program folders from `programs/`.
[PROGRAMS.TXT](PROGRAMS.TXT) describes the programs.

## Building

Needs the Pico SDK (2.3 or later) and the usual ARM toolchain.

    git clone --recursive https://github.com/ctorkildson-netizen/picocalc-virtualt
    cd picocalc-virtualt
    export PICO_SDK_PATH=/path/to/pico-sdk
    mkdir build && cd build
    cmake -G Ninja .. && ninja

The result is `build/picocalc-virtualt.uf2`; copy it to the card as
`pico2-apps/virtualt.uf2`.

## What's where

- `pico_main.c`, `vt_config.c` -- the PicoCalc side: screen, keyboard, sound,
  SD card, library, saving, clock.
- `core/` -- VirtualT's 8085 CPU, I/O and ROM tables, with changes for this
  port. BSD licence, see [LICENSE-VirtualT.txt](LICENSE-VirtualT.txt).
- `modules/picocalc-text-starter` -- Blair Leduc's PicoCalc drivers
  (submodule, pinned to a release).
- `modules/picocalc-shell` -- Blair Leduc's shell, for its return-to-shell
  header (submodule, pinned to a release).
- `programs/m100`, `programs/m200` -- the included programs, mostly from the
  CompuServe M100SIG library ([web8201.net](https://www.web8201.net)), with
  their authors' names and comments intact; a `2026:` line marks any change.
  If you wrote one of these and would rather it weren't included, open an
  issue and it will be removed.

## Licence

BSD 2-Clause for the port ([LICENSE](LICENSE)); BSD for the VirtualT core
([LICENSE-VirtualT.txt](LICENSE-VirtualT.txt)); Blair Leduc's submodules and
the included programs keep their own terms. [THIRD-PARTY.md](THIRD-PARTY.md)
lists everything taken from elsewhere, with its licence.

## Credits

VirtualT: Stephen Hurd, Ken Pettit and contributors. PicoCalc drivers and
shell: Blair Leduc. The programs: their authors, named in each file.
