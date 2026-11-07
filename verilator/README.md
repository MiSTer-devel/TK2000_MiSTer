# TK2000 Verilator simulation

```bash
cd verilator
make                 # builds obj_dir/Vemu (run from this directory: ROM .hex paths are relative)
make nibtool         # host tool for the DSK<->NIB codec
```

## Running

```bash
./obj_dir/Vemu                                   # GUI (ImGui window)
./obj_dir/Vemu --help                            # all options
./obj_dir/Vemu --headless --floppy game.dsk --stop-at-frame 1800 --screenshot-every 300
```

| Option | |
|---|---|
| `--headless` | no window (or `HEADLESS=1`) |
| `--floppy/--disk1 <f>`, `--disk2 <f>` | insert images; `.nib` raw, `.dsk/.do/.po` nibblized on the fly exactly like Main_MiSTer |
| `--mount-frame <n>` | insert drive 1 at frame n |
| `--swap <frame>:<drive>:<file>` | disk swap / eject (empty file) at a frame, repeatable |
| `--stop-at-frame <n>` | exit after frame n (60 frames = 1 s of TK2000 time) |
| `--screenshot <n,n>` / `--screenshot-every <n>` | PNGs (284x192) to `--screenshot-dir`, named `<prefix>_NNNN.png` |
| `--send-keys <frame>:<text>` | type text (`\n` Enter, `\b` BS, `\U\D\L\R` arrows, `\e` Esc, `\^X` Ctrl+X) |
| `--key-hold`, `--key-gap` | frames per key press / between keys (default 3/3) |
| `--joy <frame>:<hexmask>` | joystick bits 0 R,1 L,2 D,3 U,4 btn1,5 btn2 |
| `--reset-at-frame <n>` | pulse reset |
| `--no-disk-rom` | OSD **Machine > Disk interface: Off** (needed for tapes: otherwise the machine waits on the empty drive) |
| `--tape <f.ct2>` / `--tape-frame <n>` | load a CT2 tape at frame n; it starts when the ROM reads the tape (`LOADT`) |
| `--tape-normal` / `--tape-overlay-off` | OSD Tape speed Normal (default 7x) / Tape overlay Off |
| `--tape-swap <frame>:<file>` | load another tape later (multi-part tapes) |
| `--tape-play <n>` / `--tape-rewind <n>` | OSD `Tape play/stop` / `Rewind tape` buttons |
| `--dump-ram <frame>:<file>` | save the 64 KB main RAM at a frame |
| `--watch <lo-hi>` | log RAM writes in a hex range (frame + approx PC) |
| `--cpu-log-from <n>` / `--cpu-log-to <n>` | 6502 trace (PC, opcode, A X Y S P) to stdout |
| `--legacy-main-interleave` | old Main_MiSTer .dsk sector layout, for A/B tests |
| `--sd-ack-delay <cycles>` | SD block latency (default 1200; raise to model slow HPS) |
| `--writeback` | write floppy changes back to the image file (default: in memory only) |
| `--verbose-disk` | log every SD block |
| `--hash` | print `FBHASH frame N <hash>` every frame |
| `--cpu-log` | per-instruction 6502 log in the debug console (slow) |

Headless runs at ~17 frames/s (about 3.5x slower than real time).

## Tapes

```bash
./obj_dir/Vemu --headless --no-disk-rom --send-keys '100:LOADT\n' \
    --tape ../software/mister11/games/TK2000/Karateka.ct2 --tape-frame 250 --stop-at-frame 20000 --screenshot-every 1000
```

`LOADT` is the TK2000 command to load from tape. With the default 7x turbo
the Karateka tape (8:23 at real speed) loads in about 72 s of emulated time.

## Whole-library regression

```bash
./run_library.py ../software --filter tk2000 -i --frames 1800 -j 12 --out library_results/run1
```

Handles `.dsk/.do/.po/.nib` and `.ct2` (tapes run `LOADT` with the disk ROM off). Deduplicates images by SHA-1, runs each headless, saves screenshots + a contact
sheet per image, and writes `report.md` / `results.json` classifying each run as
`ok`, `static` (screen stopped changing early: waiting for a key, or hung),
`no-boot` (ended at the no-disk BASIC screen), `blank` or `crash`.
`--keys 900:\\n` / `--joy` pass input to every run.

## Disk path (sim == MiSTer)

`sim/a2_nib_codec.{h,cpp}` is the DSK/PO <-> NIB track codec, used both here and
in Main_MiSTer (`support/a2/`). Keep the two copies identical.
`floppy_track.sv` reads a track as 13 x 512-byte SD blocks (6656 bytes).
