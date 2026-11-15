T|out/ofgl: one process, 8 threads on 8 cores (laptop: Ryzen 7 7735U; desktop: Ryzen 7 5800X)
B|KWin 6.8.2
L|cast of the monitor, asked on the overlay's own connection (zkde_screencast_unstable_v1)
L|dmabufs, no cursor, capped at 24, 30 or half the refresh a second
E|
A|BGRx picture: 1920x1080 (laptop HDMI-A-1, 60 Hz), 3840x2160 (desktop DP-1, 120 Hz)
B|main thread: PipeWire, Wayland, GLES 3.1 (laptop cpu 14, desktop cpu 7)
L|GPU: is it a new picture (occlusion query) ─▶ copy ─▶ luma at half size ─▶ glReadPixels
L|laptop Radeon 680M 1.5 to 4.1 ms, desktop RTX 3090 Ti 0.8 to 2.2 ms
E|
A|luma bytes: 960x540 = 518,400 B (laptop), 1920x1080 = 2,073,600 B (desktop)
B|src/engine: 7 flow threads, one a core, asleep on a futex between frames
L|laptop cpus 0 2 4 6 8 10 12, desktop cpus 0 1 2 3 4 5 6
L|src/kern through vx.h: a 16-lane vector is two __m256, AVX2 and FMA3, the card's source
L|land ─▶ pyramid ─▶ histograms ─▶ search ─▶ filter ─▶ upscale ─▶ mid vectors
L|gate: laptop 960x544 1.106 ms, desktop 1920x1080 3.559 ms; 0 vectors differ from the model
E|
A|mid-instant vectors: 120x68 (laptop), 240x135 (desktop), int16 x,y; eventfd
B|main thread: the picture and its pace
L|warp shader: frames A and B moved half way along the vectors, windows over 4 blocks
L|a picture starts on a refresh and stays a whole number of them (KWin deep: frame callback
L|clock; shallow: scanout clock)
E|
A|overlay: layer shell, click-through, excluded from the cast (scripts/excl.sh)
B|the glass: generated, real, generated, real
L|24 in: 48.00 /s. laptop (60 Hz) holds 1 1 1 2, cadence 1:36 2:12; desktop (120 Hz) 2 and 3
L|30 in: 60.00 /s, holds 2 and 2. 60 in (desktop, 120 Hz): 120.00 /s, holds 1 and 1
F|
