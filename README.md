# FSR3 optical flow on a host CPU: AVX2 and FMA3, 8 threads on 8 cores

The optical flow of AMD's FidelityFX FSR 3 (the luminance pyramid, the exhaustive block search, the 3 x 3 vector filter, the upscale by SAD), as ported to a Xeon Phi's MVEX in MVEX-OpticalFLow, ported again: to any x86-64 CPU
with AVX2 and FMA3. It doubles the frame rate of one monitor: 24 to 48, 30 to 60 and 60 to 120 pictures a second (the last is half the refresh in and the refresh out, on a 120 Hz monitor). The pictures stay on the GPU; the CPU gets a luma at half size and gives the
motion vectors; the GPU moves both frames half way along them.

```
╔══ out/ofgl: one process, 8 threads on 8 cores (laptop: Ryzen 7 7735U; desktop: Ryzen 7 5800X) ═╗
║                                                                                                ║
║ ┌─ KWin 6.8.2 ───────────────────────────────────────────────────────────────────────────────┐ ║
║ │ cast of the monitor, asked on the overlay's own connection (zkde_screencast_unstable_v1)   │ ║
║ │ dmabufs, no cursor, capped at 24, 30 or half the refresh a second                          │ ║
║ └────────┬───────────────────────────────────────────────────────────────────────────────────┘ ║
║          ▼  BGRx picture: 1920x1080 (laptop HDMI-A-1, 60 Hz), 3840x2160 (desktop DP-1, 120 Hz) ║
║ ┌─ main thread: PipeWire, Wayland, GLES 3.1 (laptop cpu 14, desktop cpu 7) ──────────────────┐ ║
║ │ GPU: is it a new picture (occlusion query) ─▶ copy ─▶ luma at half size ─▶ glReadPixels    │ ║
║ │ laptop Radeon 680M 1.5 to 4.1 ms, desktop RTX 3090 Ti 0.8 to 2.2 ms                        │ ║
║ └────────┬───────────────────────────────────────────────────────────────────────────────────┘ ║
║          ▼  luma bytes: 960x540 = 518,400 B (laptop), 1920x1080 = 2,073,600 B (desktop)        ║
║ ┌─ src/engine: 7 flow threads, one a core, asleep on a futex between frames ─────────────────┐ ║
║ │ laptop cpus 0 2 4 6 8 10 12, desktop cpus 0 1 2 3 4 5 6                                    │ ║
║ │ src/kern through vx.h: a 16-lane vector is two __m256, AVX2 and FMA3, the card's source    │ ║
║ │ land ─▶ pyramid ─▶ histograms ─▶ search ─▶ filter ─▶ upscale ─▶ mid vectors                │ ║
║ │ gate: laptop 960x544 1.106 ms, desktop 1920x1080 3.559 ms; 0 vectors differ from the model │ ║
║ └────────┬───────────────────────────────────────────────────────────────────────────────────┘ ║
║          ▼  mid-instant vectors: 120x68 (laptop), 240x135 (desktop), int16 x,y; eventfd        ║
║ ┌─ main thread: the picture and its pace ────────────────────────────────────────────────────┐ ║
║ │ warp shader: frames A and B moved half way along the vectors, windows over 4 blocks        │ ║
║ │ a picture starts on a refresh and stays a whole number of them (KWin deep: frame callback  │ ║
║ │ clock; shallow: scanout clock)                                                             │ ║
║ └────────┬───────────────────────────────────────────────────────────────────────────────────┘ ║
║          ▼  overlay: layer shell, click-through, excluded from the cast (scripts/excl.sh)      ║
║ ┌─ the glass: generated, real, generated, real ──────────────────────────────────────────────┐ ║
║ │ 24 in: 48.00 /s. laptop (60 Hz) holds 1 1 1 2, cadence 1:36 2:12; desktop (120 Hz) 2 and 3 │ ║
║ │ 30 in: 60.00 /s, holds 2 and 2. 60 in (desktop, 120 Hz): 120.00 /s, holds 1 and 1          │ ║
║ └────────────────────────────────────────────────────────────────────────────────────────────┘ ║
║                                                                                                ║
╚════════════════════════════════════════════════════════════════════════════════════════════════╝
```

## The law of the 8 cores

- `OF_MAX_THREADS` is 8, a compile-time number (`src/engine/cpus.h`). No option raises it.
- Before anything else exists the process confines itself to one CPU of each of at most 8 physical cores (`of_cpus_pick`: the fastest cores first, never two hardware threads of one core). Every thread it will ever own
  inherits that.
- The flow gets 7 threads, each pinned to a core of its own (`src/engine/team.c`); the program's own thread (GL, Wayland, PipeWire) is the eighth, on the core that is left. The gate runs the flow on 8 (the caller is one).
- Between frames the flow's threads sleep in the kernel (a futex after 50 microseconds of spinning): a still screen costs nothing.
- Measured, not assumed: the program prints the threads in the process. With the NVIDIA driver: 8. With Mesa (radeonsi): 16, of which 8 are this program's and 8 the driver's own helpers, confined to the same 8 cores.

## How the port was made

`src/common/vx.h` is the vocabulary every kernel is written in: one name per 16-lane operation. On the card each name was one MVEX instruction. Here each has two bodies:

- the native body: a 16-lane vector is two `__m256`; each operation is its AVX2 or FMA3 instruction on both halves; a fused multiply-add stays one rounding; MVEX's mask registers are a 16-bit number expanded to lane masks.
- the lane model (`-DVX_MODEL`): plain C, one IEEE operation a lane.

The kernels in `src/kern` are the card's files. Two were touched: `framegen.c` (the search names its window by `vx_window`, which the host makes by loading from the address itself where the card slid two registers) and
`amdfi.c` (one permute given a name). The gate links both bodies into one program (the model's symbols under the prefix `model_`, by a header generated from the native objects' own symbol tables) and holds them to each other.

## Use

```
make                          # out/ofgate, out/ofgl, out/moving; -march=x86-64-v3, so what is built on one machine runs on another
out/ofgate --size 960x544 --threads 7     # the gate: native = model, truth, times
scripts/authorize.sh          # once per machine: KWin gives its monitor cast to a program whose .desktop file asks for it
echo 'OFGL_OUTPUT=HDMI-A-1' > local.conf   # the monitor's name; OFGL_ARGS="24" makes 24 the default
scripts/ofgl.sh start 24      # 24 in, 48 out
scripts/ofgl.sh start 30      # 30 in, 60 out
scripts/ofgl.sh start 60      # 60 in, 120 out, on a 120 Hz monitor (119.88 counts: 59.94 in); a slower monitor is refused, with what it can show
scripts/ofgl.sh start native  # half the refresh in, the refresh out, whatever the refresh is: the default, and what 60 is on a 120 Hz monitor
scripts/ofgl.sh plan 60       # the command line start would run with the same words; starts nothing
scripts/ofgl.sh status        # the last second's line
scripts/ofgl.sh stop          # the monitor is plain again
```

`out/ofgl --trace` prints a line for every picture that reached the glass: how many refreshes after the one before, and how deep KWin was.

Needs: KWin on Wayland (the cast and the capture exclusion are KWin's), PipeWire, EGL with GLES 3.1 and dmabuf import, a CPU with AVX2 and FMA3. Another compositor: `--node N` takes a cast made some other way, and
`--no-excl` leaves out the exclusion, which then has to come from elsewhere or the overlay films itself.

## Measured

Gate, 24 frames of a scene with known motion, medians, the native body and the model in turn on the same threads (`logs/gate/`):

| machine, luma, threads | native ms | model ms | vectors that differ | every block searching |
|---|---|---|---|---|
| desktop (Ryzen 7 5800X), 1920x1080, 8 | 3.195 to 3.288 | 6.199 to 6.344 | 0 of 745,200 | 7.065 to 7.222 |
| desktop, 1920x1080, 7 | 3.559 to 3.636 | 7.067 to 7.224 | 0 of 745,200 | 7.913 to 8.139 |
| desktop, 960x544, 7 | 0.870 to 0.874 | 1.875 to 1.896 | 0 of 187,680 | not run |
| laptop (Ryzen 7 7735U, on battery), 960x544, 7 | 1.106 | 2.132 | 0 of 187,680 | 2.414 |
| laptop, 1920x1080, 8 | 8.428 | 11.337 | 0 of 745,200 | not run |

The card this came from (Xeon Phi 7120A, 118 threads) took 11.181 ms for the 1920x1080 luma. Truth: 99.92 to 99.97 % of ground blocks and 99.90 to 100 % of box blocks exact. The control (one vector flipped) was caught in
every field of every run.

Live, on the glass: the pattern test (`tools/moving` under the overlay), 9 s a run, the flow on 7 threads (`logs/live/`; what was tried for the pace is in `docs/measurements.md`):

| in, out | monitor | on the glass | a generated picture, a real frame | flow ms a frame | discarded |
|---|---|---|---|---|---|
| 24, 48 | desktop, 3840x2160 at 120 Hz | 48.00 /s in 8 of 9 s | 2 refreshes, 3 refreshes | 4.56 to 4.95 | 0 |
| 30, 60 | desktop, 3840x2160 at 120 Hz | 60.00 /s in 6 of 9 s, 56 to 59 in 3 | 2 and 2 (529 of 535 pictures) | 5.00 to 5.68 | 0 |
| 60, 120 | desktop, 3840x2160 at 120 Hz | 120.00 /s in 8 of 9 s, 115.00 in the first | 1 and 1 (960 of 960 pictures in those 8 s) | 5.84 to 6.51, 9.83 at most | 0 |
| 24, 48 | laptop, 1920x1080 at 60 Hz, its own content, 125 s | 47.15 /s in the mean | 1 1 1 2 | 1.69 in the last second | 40 |

60 in, 120 out is the hardest of the three: 16.667 ms from one source frame to the next, and a 3840x2160 monitor's luma is 1920x1080. The flow took 5.84 to 6.51 ms of that in the mean of each second and 9.83 ms at
most; no frame was late, none skipped, no picture discarded; the latency the program prints (a shown picture's scanout less its source frame's time) was 28.6 to 29.0 ms in the mean of each second. The card this came
from took 12.76 ms a frame in the mean for the same luma (MVEX-OpticalFLow's README). `scripts/ofgl.sh start 60` runs exactly what was measured: on a 120 Hz monitor it is the program with no `--rate`, one picture a
refresh, the code that descends from that project's 60 in, 120 out program. The program's own `--rate 60` is refused at 119.88 Hz (120 out is more than the monitor shows), which is why the script decides.

## Not overcome

- No eye but the owner's has judged the picture; the overlay is excluded from capture, so no program can.
- The laptop still loses a generated picture now and then: 40 discarded in 125 s at 24 in (47.15 pictures a second for 48; 6 in 39 s in the run before). KWin there composes late in the refresh, and a picture sent at the wrong moment replaces one not yet shown.
- 24 in on a 60 Hz monitor is 48 pictures in 60 refreshes: three pictures of one refresh and one of two, by arithmetic. Even pacing needs a 48 Hz or 120 Hz mode.
- The search is 60 to 82 % of the flow's time and is still the card's lane form. Its windows are bytes times 64 at the finest level: `vmpsadbw` on the bytes would do 8 places an instruction. Not built.
- The picture forms of the source project (pictures through the CPU: fused, plain, pixel, FSR, deblocking) compile under the native body and are linked, and nothing runs them here: not gated.
- A compositor other than KWin has no cast and no exclusion from this project.
- 60 in, 120 out was run for 9 s under the pattern test, not for minutes under a game. On a monitor faster than 120.6 Hz `start 60` takes `--rate 60` (each picture held whole refreshes): no such monitor was here, so
  that branch was never on a glass. The branches that were decided on real monitors: 120.000 Hz and 119.879 Hz (native), 59.790 Hz (refused).

## Where things came from

`docs/PROVENANCE.sha256`: every file copied from MVEX-OpticalFLow (commit 7fad41a) with its hash. `tools/port/`: the generators that made `src/host/ofgl.c` from that project's `mvexgl.c`. `LICENSES/`: AMD's MIT licence for the
FidelityFX shaders the search, filter and upscale derive from. `tools/protocols/zkde-screencast-unstable-v1.xml`: KDE, LGPL-2.1-or-later. The other files of `tools/protocols/` are the Wayland protocols under their own notices.

Not tracked: `out/` (what `make` builds), `logs/` (each machine's own records, named where a number is given), `local.conf` (the machine's monitor).
