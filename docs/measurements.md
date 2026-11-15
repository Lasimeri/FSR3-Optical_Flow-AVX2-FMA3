# Measurements

Every number here has its log, named beside it; `logs/` is each machine's own record and is not in the repository. Desktop: Ryzen 7 5800X, RTX 3090 Ti (NVIDIA 615.78.08), KWin 6.8.2, DP-1 at 3840x2160, 120 Hz. Laptop: Ryzen 7 7735U, Radeon 680M (Mesa 26.2.6), KWin 6.8.2, HDMI-A-1 at 1920x1080,
60 Hz, on battery. GCC 16.2.1, `-O2 -march=x86-64-v3`; the two machines built bit-identical binaries (sha256 of out/ofgl and out/ofgate compared, 2026-11-15).

## The gate

`logs/gate/20261115-002717-desktop-final.txt` (two rounds of five runs, in turn; 10 of 10 PASS), `logs/gate/20261115-000124-base-vxbody.txt`, and on the laptop `logs/gate/20261115-001046-laptop-first.txt`. The table is in the
README. Stage medians, desktop, 1920x1080, 8 threads, microseconds: land 304 to 322, pyramid 140 to 195, histograms 39, search 1964 to 2005, filter 159 to 162, upscale 276 to 289, mid vectors 227 to 257.

## Live, the pattern test (tools/moving under the overlay)

| run | log | glass | cadence | flow ms | discarded |
|---|---|---|---|---|---|
| desktop, 24 in | logs/live/20261115-002524-depth-24.txt | 48.00 /s in 8 of 9 s | 2:24 3:24 | 4.56 to 4.95 | 0 |
| desktop, 30 in | logs/live/20261115-002510-depth-30.txt | 60.00 /s in 6 of 9 s, 56 to 59 in 3 | 2:60 | 5.00 to 5.68 | 0 |
| desktop, 60 in (`start 60`: native at 120 Hz) | logs/live/20261115-002538-depth-native.txt | 120.00 /s in 8 of 9 s | 1:120 | 5.84 to 6.51 | 0 |
| laptop, 24 in, its own content | logs/live/20261115-002611.log (on the laptop) | 47.44 /s mean over 39 s | 1:36 2:12 | 1.58 to 2.27 | 6 |
| laptop, 24 in, its own content | logs/live/20261115-002708.log (on the laptop) | 47.15 /s mean over 125 s | 1:36 2:12 | 1.69 in the last second | 40 |

Desktop, 30 in: 529 of 535 pictures lasted 2 refreshes; KWin moved between its two depths during the run (70 pictures shallow, 465 deep) and the six others are at those moves and at the start.

## The pace: what was tried, in order

1. Timing the next picture from the scanout instant of the one on the glass plus 0.3 ms. Desktop, 30 in: pictures of 1 and 3 refreshes for 2 and 2 (`logs/live/20261115-001301-pattern-30.txt`, cadence 1:30 2:0 3:30 in a second).
   Cause, from the trace (`logs/live/20261115-001534-trace-30.txt`): KWin was composing right after the vblank, so a commit made 0.3 ms after it reached the glass 16 ms later, not 8.
2. wp_fifo barriers, each held refresh a commit that changes nothing. The next picture sent on the frame callback: generated pictures discarded (laptop: 5 to 10 a second in some seconds). The next picture sent once the
   one before was presented: no discards, and every picture held 1 refresh too long whenever KWin was deep (`logs/live/20261115-002014-fifo2-24.txt`: 64 pictures of 4 refreshes, 4 to 5 generated pictures skipped a second).
3. The frame callback's instant plus whole refreshes. Desktop: 596 of 597 pictures exact at 30 in. Laptop: generated pictures discarded again, more each second (2, 1, 4, 6, 7, 7, 6, 10): KWin there is shallow, and the frame
   callback can come while the picture still waits.
4. What is in the source now: both clocks, chosen by how long the last picture took from its draw to the glass (over 7/6 of a refresh: deep, the frame callback clock; under: shallow, the scanout clock).
