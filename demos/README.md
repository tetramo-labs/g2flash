# g2 CFW demos

Two small [Bun](https://bun.sh) programs that talk to a pair of Even Realities
G2 glasses over Bluetooth and show off the [custom firmware](../) built by
`../build_cfw.sh`:

- **`detect-cfw.ts`** — reads the glasses' settings and reports whether they're
  running the custom firmware, and which extensions it advertises. Works on
  stock firmware too (it just reports "no CFW").
- **`ring-touch-test.ts`** — live ring touch-down acceptance test. Run
  `bun ring-touch-test.ts --timeout 120 --count 3`, wait for `READY`, then
  touch and release the ring three times about two seconds apart. It connects
  to both lenses, opens an EvenHub page, acquires both framebuffer leases, and
  prints decoded input plus raw protobuf and timestamped `RI` ring reports.
  Only ring touch-down (SysEvent 14, raw wire type 10 when present) counts as
  success; taps, long presses and releases are logged separately. Exits 1 on
  timeout and releases the leases, dismisses the page and disconnects on exit
  or Ctrl-C. Requires existing CFW; does not build or flash firmware.
- **`video-bench.ts`** — streams a video (as a GIF) to the lens as fast as it
  acks and benchmarks the achieved framerate / byte count. Streams via the
  CFW's compressed packed-4bpp keyframe and delta modes.
- **`shapes-demo.ts`** — draws a vector test card with mode 36, then builds a
  retained scene (mode 37) and lets the firmware animate it: eased glides and
  tweens of geometry, color and stroke width, driven by one small message per
  transition. Needs the `glassly-cfw` build, revision 26 or later.
- **`card-demo.ts`** — card-on-top with depth on the object cache: content
  objects stay on the list while a card painted after them slides in from the
  right at a nearer depth, glides out, and pops back as pure references. Depth
  on the G2 is stereo disparity, so the card's objects go out in two
  lens-targeted SHOWs (x + d on the left lens, x − d on the right); the phone
  SDK does the same per-lens split for elements with a `depth`.
  `G2_DEPTH=10 bun card-demo.ts` picks the disparity. Revision 38 or later.

- **`ble-mode.ts`** — switches the BLE link between the stock connection
  profile (default) and the 7.5 ms fast profile, or queries it:
  `bun ble-mode.ts [stock|fast|query]`. Sends sid 0x09 field 127 to both
  lenses and prints the field-128 status from each reply.
- **`shapes-suite.ts`** — the glassly example-miniapp shapes test suite (every
  `render()` shape element plus the transition contract, 43 cases) run straight
  against the glasses. The phone's render pipeline and its object-cache encoder
  are ported into the script, so it prints the same pass/fail verdicts as the
  miniapp's tester page, plus per-case ack and render timings and how many
  objects the glasses hold.
- **`object-cache.ts`** — the reference encoder for the revision-38 object cache
  (modes 37-41): shape records, PUT/SHOW/HIDE/STATE/CONTROL messages, ops,
  reply parsing. `cfw-transport.ts` adds `CacheLink`, which owns the session
  epoch and turns the kind-5 replies into results.
- **`bad-apple-tests.ts`** — the example miniapp's three Bad Apple tests
  (`--text`, `--bitmap`, `--shapes`): the same clip played the three ways a
  miniapp can animate, sent as what the phone puts on the air for each — the
  raster path for the text wall and the image element, mode-37 tweens for the
  rects — with per-mode wire bytes, achieved framerate and skipped frames.

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
bun shapes-suite.ts --self-test        # encoder vs the phone's unit-test vectors
G2_DRY_RUN=1 bun shapes-suite.ts       # host pipeline only, no glasses
G2_TRACE=1 bun shapes-suite.ts         # per-frame op counts, bytes, ack times
G2_HOLD_SCALE=0.5 G2_OUT=results.json bun shapes-suite.ts
```

Each case renders its frames in order, one SHOW per frame, then judges
the last frame's `dropped` / `degraded` report against the case's expectation
for a 576×288 canvas that draws every shape and animates. `G2_OUT` writes the
per-case results as JSON. The verdicts come from the ported phone-side logic;
what the panel actually shows is yours to eyeball, which is why the holds are
there (`G2_HOLD_SCALE=0` skips them).

Pacing: a step's frame goes out `holdMs` after the previous frame went out
(never before that frame was acked), so a frame stays up for about its hold and
no frame waits for an earlier frame's transitions — captions scroll while they
glide, the stage-5 readout ticks beside the tweening gauge, and a second glide
lands mid-flight in the compose cases. Only a case's last frame waits for its
transitions before the hold and the blank. The session starts with a hidden
two-frame tween so the first visible transition is not also the firmware's first
animation.

Every TWEEN carries the element's full tweenable geometry (plus color and stroke
width), not only the parameters that changed: the firmware takes an unmasked
parameter's end value from the object's current, possibly mid-flight, value, so a
partial mask would freeze that axis when a second move lands during the first.

Objects are the unit of caching: every element slot has a stable id derived
from the element id, and a frame is one SHOW of (id, version) references plus
the PUTs for new or changed definitions and the TWEENs for animated changes.
An element that leaves a frame stays on the glasses; when it comes back it is
a pure reference (`G2_TRACE=1` prints the per-frame put/cached/tween counts).
A blank frame is a HIDE; objects hidden mid-tween are re-snapped when shown
again.

### Replaying the suite through the firmware code

`--dump` writes every payload, the wait between them and a label per case;
`patches/host/vector_host_test.c` pushes that stream through the real
`scene.c` on the host (filling in the session epoch and request ids the way
the link does), ticking the animation timer through each wait, and reports
every message the firmware refuses.

```bash
bun shapes-suite.ts --dump /tmp/suite.bin
cd ../..
cc -std=c11 -O1 -Wall -Wno-unused-function -Ipatches -o /tmp/vector_host_test patches/host/vector_host_test.c
/tmp/vector_host_test /tmp/out /tmp/suite.bin
```

## Bad Apple tests

For filled SVG paths and animated rotation on the object cache, use:

```sh
bun run test:vectors                         # offline C + TS + video/pixel suite
bun vector-suite.ts --device                 # direct BLE visual suite
bun vector-suite.ts --bad-apple --device      # vector video, clocked at 10 fps
bun vector-suite.ts --bad-apple --svg-out /tmp/bad-apple-svg
```

The smooth version uses the [Alstroemeria Records YouTube upload](https://www.youtube.com/watch?v=i41KoE0iMYU)
(downloaded at 1444×1080), traced with Potrace at 576×432. To prepare it,
install `yt-dlp`, `ffmpeg` and `potrace`, then run from `demos`:

```sh
mkdir -p .cache/bad-apple
yt-dlp --ignore-config --no-playlist -f 'bv*[height<=1080]+ba/b[height<=1080]' --merge-output-format mp4 --write-info-json -o '.cache/bad-apple/source.%(ext)s' 'https://www.youtube.com/watch?v=i41KoE0iMYU'
bun prepare-bad-apple.ts
bun vector-suite.ts --bad-apple --device
```

Preparation checks every Bézier path with the actual firmware C compiler.
Complex frames are split into tiles to fit 512 edges per path, keeping the
576×432 sampling and the original 4:3 proportions. All tiles replace the scene
atomically. The default is 300 frames at 10 fps; preparation accepts `--frames`,
`--fps`, `--input` and `--potrace`. A locally built Potrace at
`.cache/potrace/bin/potrace` is also detected. Downloads and generated assets
stay in the ignored `.cache` directory.

`--bad-apple` prefers `.cache/bad-apple/smooth/video.json` when present.
`--svg-video FILE` selects a prepared video explicitly; `--gif bad_apple_quarter.gif`
selects the original bitmap contour version. The offline test suite always tests
the original fixtures and also replays the prepared smooth video when present.
The first 300 smooth frames use at most 5 paths and 1,156 total compiled edges;
payloads average 1,471 bytes (95th percentile 2,785, maximum 3,641).

`vector-suite.ts` is offline unless `--device` is supplied. It gates on
`EVENCFW/21`, and uses the existing g2-kit connection configuration. No mobile
app is involved. Lease acquire, renew and release include a basic-settings
read so the firmware sends a reply; a lease control field alone is silent.
The video consists of filled compound contours, including
holes, instead of tracked rectangles. See
[`patches/VECTOR_PROTOCOL.md`](../patches/VECTOR_PROTOCOL.md) for the binary
contract, limits, host replay, and expected visual checks. Device ACKs alone
do not verify pixels. The existing three-mode benchmark below is unchanged.

```bash
bun bad-apple-tests.ts                       # text, bitmap, shapes in turn
bun bad-apple-tests.ts --shapes              # one mode (or several, in order)
bun bad-apple-tests.ts --shapes --loops 3    # play the clip three times
bun bad-apple-tests.ts --frames 100 --fps 15 # shorter, faster
bun bad-apple-tests.ts --dry-run             # decode + encode + report, no glasses
bun bad-apple-tests.ts --trace               # per-frame wire bytes and ack times
bun bad-apple-tests.ts --gif other.gif       # another grayscale GIF
```

The clip is `bad_apple_quarter.gif` sampled exactly as the miniapp's
`scripts/generate-bad-apple.ts` samples it (156×80, 1-bit, 10 fps, 300
frames), so it is the miniapp's clip. Each mode mirrors how the phone delivers
that miniapp render on the CFW:

| Mode | The miniapp sends | On the air |
|------|-------------------|------------|
| `--text` | one `font:"mono"` text element of ▀ ▄ █ half-blocks, 78×40 cells | the phone rasterizes mono text outside mode 14's ASCII range, so a mode-6 keyframe then mode-3 bounding-box deltas |
| `--bitmap` | one 156×80 4bpp BMP image element per frame (~9 KB base64) | a full-canvas tile misses the 64 KiB texture cache, so the same raster path at finer pixels |
| `--shapes` | ≤80 filled rects with stable ids and a one-frame linear transition | mode-37 patches: new rects SET, moved rects TWEEN, vanished rects DELETE; the glasses animate the silhouette |

Every mode plays by the clock and skips the frames the ack cadence cannot keep
up with, as the miniapp's player does, so the achieved framerate and the
skipped count are the measurement. The summary also shows the bytes the
miniapp would push across the phone bridge per frame, next to the wire bytes.

## Requires the custom firmware

`video-bench.ts` and `bad-apple-tests.ts` use display modes that only exist in
the CFW; against stock firmware they won't render. Build and flash the firmware first (see the
[top-level README](../README.md)), then confirm with `detect-cfw.ts`.
