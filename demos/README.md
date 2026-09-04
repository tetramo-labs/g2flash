# g2 CFW demos

Two small [Bun](https://bun.sh) programs that talk to a pair of Even Realities
G2 glasses over Bluetooth and show off the [custom firmware](../) built by
`../build_cfw.sh`:

- **`detect-cfw.ts`** — reads the glasses' settings and reports whether they're
  running the custom firmware, and which extensions it advertises. Works on
  stock firmware too (it just reports "no CFW").
- **`video-bench.ts`** — streams a video (as a GIF) to the lens as fast as it
  acks and benchmarks the achieved framerate / byte count. Streams via the
  CFW's compressed packed-4bpp keyframe and delta modes.
- **`shapes-demo.ts`** — draws a vector test card with mode 16, then builds a
  retained scene (mode 17) and lets the firmware animate it: eased glides and
  tweens of geometry, color and stroke width, driven by one small message per
  transition. Needs the `glassly-cfw` build (`shapes16 scene17 anim18`).
- **`shapes-suite.ts`** — the glassly example-miniapp shapes test suite (every
  `render()` shape element plus the transition contract, 43 cases) run straight
  against the glasses. The phone's render pipeline and its mode-17 scene encoder
  are ported into the script, so it prints the same pass/fail verdicts as the
  miniapp's tester page, plus per-case ack and render timings.

They depend on [`g2-kit`](https://github.com/jimrandomh/g2-kit-unofficial) (a
reverse-engineered BLE library for the G2), pulled directly from GitHub — see
`package.json`. Nothing here needs the rest of this repo at runtime; the CFW
just needs to already be flashed for the demos to show anything interesting.

## Setup

```bash
cd demos
bun install
```

> The glasses must be powered on and **not** connected to the phone (quit the
> Even app / turn off the phone's Bluetooth) so they advertise for a direct
> connection. On macOS the first run prompts for Bluetooth permission.

## Detect the firmware

```bash
bun detect-cfw.ts        # or: bun run detect
```

On the custom firmware you'll see something like:

```
firmware: L=2.2.4.34 R=2.2.4.34
CFW detected: EVENCFW/6 img576 img640 imgz rle wakelease directfb fbguard
  contract v6, features: img576, img640, imgz, rle, wakelease, directfb, fbguard
  img576=yes img640=yes imgz=yes rle=yes directfb=yes fbguard=yes
```

On stock firmware it prints `no CFW capability field`.

## Video streaming benchmark

`video-bench.ts` takes a GIF and streams its frames. Make one from any video
with ffmpeg (grayscale, sized to the lens):

```bash
ffmpeg -i input.mp4 -vf "fps=30,scale=288:144:flags=area,format=gray" demo.gif
bun video-bench.ts demo.gif        # or: bun run bench demo.gif
```

It decodes/rescales/compresses every frame up front, then streams them in
order, pacing on the per-fragment acks, and prints framerate + bytes at the end.

Useful environment variables:

| Var | Default | Meaning |
|-----|---------|---------|
| `G2_IMG_W` / `G2_IMG_H` | `640` / `480` | target size (CFW modes are fixed at `640`×`480`; `lz4` defaults to `288`×`144`) |
| `G2_IMG_THRESHOLD` | `-1` | `>=0` = 1-bit threshold; `-1` = grayscale |
| `G2_MODE` | `delta` | `delta` = mode-6 keyframe plus mode-3 bounding-box updates; `raw4` = mode-6 full frames; `lz4` = stock compressed BMP. CFW modes RLE pixels before deflate. |
| `G2_KEYFRAME_INTERVAL` | `0` | in `delta` mode, force a full frame every N |
| `G2_FRAME_STRIDE` | `1` | use every Nth source frame |
| `G2_MAX_FRAMES` | `0` | cap frame count (`0` = all) |
| `G2_WINDOW` | `2` | image messages in flight at once (`1` = serial) |
| `G2_DRY_RUN` | — | `1` = decode/compress/report only, don't connect |

Sweep `G2_WINDOW` (e.g. `1`, `2`, `4`) to see how much the ack round-trip is
costing — higher windows overlap the next frame's BLE transfer with the current
frame's on-device processing.

## Shapes test suite

```bash
bun shapes-suite.ts                    # every case
bun shapes-suite.ts rect animation     # by group
bun shapes-suite.ts anim-glide         # by id
bun shapes-suite.ts --list             # ids and groups, no connection
G2_DRY_RUN=1 bun shapes-suite.ts       # host pipeline only, no glasses
G2_HOLD_SCALE=0.5 G2_OUT=results.json bun shapes-suite.ts
```

Each case renders its frames in order (one mode-17 patch per frame, awaited
until the glasses ack it and its transitions have played), then judges the last
frame's `dropped` / `degraded` report against the case's expectation for a
576×288 canvas that draws every shape and animates. `G2_OUT` writes the
per-case results as JSON. The verdicts come from the ported phone-side logic;
what the panel actually shows is yours to eyeball, which is why the holds are
there (`G2_HOLD_SCALE=0` skips them).

## Requires the custom firmware

`video-bench.ts` uses display modes that only exist in the CFW; against stock
firmware it won't render. Build and flash the firmware first (see the
[top-level README](../README.md)), then confirm with `detect-cfw.ts`.
