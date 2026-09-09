# Revision 21: filled paths and rotation

Revision 24 moves the graphics transport from modes 16/17/18 to **36/37/38**
to avoid upstream sensor commands. The path and rotation operations introduced
in revision 21 keep their record formats and opcodes (8 and 9). Check
`EVENCFW/24` or later and the `scene37` capability before sending these packets.
Modes 37/38 remain standalone messages, not mode-8 batch children.

The glasses render retained vector paths themselves. The wire input is compiled
geometry, not SVG XML. `demos/vector-protocol.ts` converts the supported SVG path
syntax into this format, independently of Glassly/mobile.

## SET_PATH (operation 8)

Inside `[37][scene flags][background][operations...]`:

```text
[8:u8][slot:u8][visible:u8][color:u8][fill-rule:u8]
[x:i16][y:i16][scale:u16][length:u16][commands:length bytes]
```

All multibyte values are little endian. Slot is 0–127. Visible is 0 or 1;
color is 0–15. Fill rule 0 is nonzero winding, 1 is even-odd. The complete set
of contours in a path is filled together, so holes and overlapping contours
follow the selected rule. Paths paint in the same slot order as other shapes.

`x,y` translate the asset in panel pixels. Scale is uniform Q8: 256 means 1×,
512 means 2×; valid values are 0–2048 (0×–8×). Scaling acts around the asset's
local origin, then translation is applied, then the slot's rotation.

Commands use signed **Q4 coordinates** (1/16 pixel, int16; -2048 through
2047.9375), in the asset's local coordinate space:

| Opcode | Coordinates following the opcode |
| --- | --- |
| 0: M | x, y |
| 1: L | x, y |
| 2: Q | control x/y, endpoint x/y |
| 3: C | first control x/y, second control x/y, endpoint x/y |
| 4: Z | none |

Every contour starts with M and explicitly closes with Z. Commands between
contours must start a new M. A degenerate `M0 0Z` is a valid empty asset.
The compiler flattens curves once at upload, using bounded subdivision and a
roughly half-pixel local-space tolerance. Scaling also scales that tolerance;
it is not adaptive screen-space tessellation during animation. No antialiasing
is applied. Paths currently support solid fills, not strokes, gradients,
filters, clipping paths, embedded SVG animation or arbitrary path morphing.

Limits, enforced before applying the scene:

- 8,192 command bytes and 1,024 commands per path.
- 512 flattened edges per path; subdivision depth at most 8.
- 4,096 retained edges across the final scene, plus at most 4,096 staged edges
  per incoming message. Each edge occupies 16 bytes.
- One SET_PATH per slot per message. A message may replace several distinct
  path slots atomically. The all-slots index 255 is not valid for SET_PATH.

SET_PATH replaces that slot, stops its old animations and resets its rotation
to zero. Geometry is immutable until another SET_PATH replaces it. The path is
represented internally as shape type 18; type 18 cannot be created by an
ordinary SET or immediate mode-36 shape record.

Paths use the existing MOVE/GLIDE, SHOW, DELETE, FREEZE and FINISH operations.
For their legacy TWEEN operation, only these mask bits are valid:

| Bit | Parameter |
| --- | --- |
| 0 | p0: x translation, signed pixels |
| 1 | p1: y translation, signed pixels |
| 2 | p2: uniform Q8 scale, 0–2048 |
| 8 | color, 0–15 |

Geometry/color tweens keep the existing frame-count semantics. A path's
control points do not morph when a new asset replaces it.

## ROTATE (operation 9)

```text
[9:u8][slot:u8][angle:i32][pivot-x:i16][pivot-y:i16]
[duration-ms:u16][curve-x1:u8][curve-y1:u8][curve-x2:u8][curve-y2:u8]
```

This is a 16-byte operation. Angles are **degrees × 256**, positive clockwise.
Allowed targets are -36,000 through +36,000 degrees (100 turns either way).
Angles remain unwrapped during interpolation: 0→360 is one complete turn;
350→10 goes backward 340 degrees. For a short forward turn, send 370 instead
of 10. Exact full-turn angles use the original primitive rasterizer.

The pivot is explicit, in **panel pixels**, and stays fixed when geometry moves.
For a moving-center rotation, update the pivot along with the application's
position targets; for an orbit, leave the pivot fixed. Pivot changes apply
immediately; the pivot itself does not tween. Use the same pivot for every slot
when rotating a multipart SVG or a group of shapes together.

Duration 0 applies immediately; 1–65,535 animates over that many milliseconds.
The easing curve has the existing CSS cubic-bezier byte encoding (coordinates
0–255). Rotation uses elapsed OS time, including across tick wraparound. A
missed display tick catches up on the next available presentation. Frame period
still controls presentation cadence, so a 1 ms animation does not imply a 1 ms
display refresh.

Rotation is an independent animation channel: sending GLIDE/TWEEN does not
cancel rotation, and retargeting rotation does not cancel a geometry tween.
Retargeting starts at its elapsed current angle. FREEZE/FINISH affect both
channels; ordinary SET/SET_PATH/DELETE reset both. FREEZE's existing display
commit semantics still apply. Under framebuffer/timer allocation failure,
animations snap to their final state for static presentation.

Supported targets are existing geometric shapes 1–13 and retained paths 18.
Rotation of an empty slot, image, cached text or inline text is rejected before
any operation in that message applies. Slot 255 is not accepted. Text/image
sampling remains axis-aligned. Transformed geometry is generated from the
original coordinates each draw to avoid accumulating rounding error.

## Transaction and lifetime

The entire message's structure, path geometry, target types, reserved fields
and final edge budget are validated before the scene changes. All replacement
paths are allocated first. Failure frees staging allocations and retains the
previous slots, colors, background, animations and displayed frame. Lazy scene
or scratch workspace allocation can remain cached after a rejected message.

Successful replacement happens under the display gate. The previous path is
freed only when the replacement is ready; the display task cannot see an
incomplete upload. This also permits CLEAR plus replacements in one message.

DELETE, CLEAR, SET replacement, SET_PATH replacement, mode 38 release and mode
11 cleanup free the paths they supersede. Scratch workspace is about 10.5 KiB,
allocated on first path/rotation use, and freed on scene release. The extended
slot table plus inline text uses about 28 KiB; the framebuffer remains 150 KiB.
The maximum live plus staged edge storage is 128 KiB, in addition to those
allocations and any texture cache. Actual available heap is hardware-dependent.

Framebuffer lease expiry/release stops animation and rejects new scene writes;
as with the previous retained scene implementation, scene storage is kept
until explicit mode 38 release or mode 11 cleanup. Lease callbacks do not own
the display gate and must not free buffers the display task may still use.
Clients should release the scene before releasing the lease, and replay the
scene after reconnecting rather than assuming an old baseline.

## Standalone validation and demos

From the repository root, after installing `demos` dependencies:

```sh
python3 patches/host/run_vector_tests.py --out /tmp/g2-vector-tests
```

The runner builds the C tests with AddressSanitizer/UndefinedBehaviorSanitizer,
runs old shape regressions and new tests, checks TypeScript, compiles the exact
device payloads, replays them against firmware C, checks pixel probes, generates
PGM reference frames and SVG video frames, and cross-compiles the complete
Thumb patch. `--no-sanitize` is available for toolchains without sanitizers.

```sh
cd demos
bun vector-suite.ts --help
bun vector-suite.ts --device
bun vector-suite.ts --bad-apple --device
bun vector-suite.ts --bad-apple --svg-out /tmp/bad-apple-svg
```

Offline operation is the default. `--device` connects using the same `g2-kit`
transport/environment configuration as `shapes-demo.ts`. It requires revision
21+, installs a carrier layout, obtains/renews the framebuffer lease, sends
the fixtures, then releases the scene and lease. ACKs confirm transport, not
visual correctness: inspect named stages against the host reference images.
No firmware flashing is performed by these commands.

The prepared Bad Apple demo uses the Alstroemeria Records YouTube source,
traced at 576×432 with Potrace cubic Bézier curves. Run
`bun prepare-bad-apple.ts` in `demos` after downloading the source; see
`demos/README.md`. Every path is checked by the actual firmware compiler via
`patches/host/vector_path_check.c`. Complex frames are subdivided into tiles
instead of reducing sampling resolution. CLEAR and all tile replacements are
sent in a single atomic scene message. The first 300 frames fit within 5 paths
and 1,156 total edges per frame. `--bad-apple` uses this prepared video when
available; `--svg-video FILE` selects it explicitly. Browser SVG rendering
antialiases edges; the firmware renderer still uses solid pixel fills.

The original GIF demo (`--bad-apple --gif bad_apple_quarter.gif`) traces exact pixel boundaries into SVG M/L/Z contours,
preserving holes rather than approximating with rectangles. It uses the bundled
GIF's existing video-style compositing assumptions and samples 10 fps, up to
300 frames by default. This is polygonal vector playback, not smooth Bezier
tracing or path morphing. It starts at 144×72 sampling and reduces resolution
only if necessary to meet the edge budget; it reports that choice. Live playback
skips late frames instead of building a queue. The compiled payload retains one
path slot per frame. Representative frames and 24 source-pixel probes per frame
are replayed on the host. The included 300 frames all fit at 144×72.

The dump format extends the old suite replay format: kind 1 + u16 length + raw
message; kind 2 + u32 wait milliseconds; kind 3 + u8 UTF-8 label length + label;
kind 4 + u16 x + u16 y + u8 expected gray; kind 5 requests a PGM snapshot.
Use `vector_host_test OUTPUT_DIR DUMP_FILE` to replay a dump; the old scene
replayer does not understand kinds 4/5.
