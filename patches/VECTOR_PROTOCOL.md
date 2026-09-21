# Filled paths and rotation

Revision 38 carries paths and rotation on the retained object cache (modes
37–41, `scene.c`): a path is a shape record of type 18 defining an object, and
rotation is an op of a mode-38 SHOW addressed by object id. Check
`GLASSLYCFW/38` or later before sending these packets. Cache messages are
also accepted inside mode-8 bundles.

The glasses render retained vector paths themselves. The wire input is compiled
geometry, not SVG XML. `demos/vector-protocol.ts` converts the supported SVG path
syntax into this format, independently of Glassly/mobile.

## PATH records (shape type 18)

Revision 38: paths are objects of the retained object cache (`scene.c`). A path
is defined by a PUT_OBJECT entry (inside a mode-37 PUT or embedded in a mode-38
SHOW) whose shape record is:

```text
[18:u8][flags:u8][color:u8][fill-rule:u8]
[x:i16][y:i16][scale:u16][length:u16][commands:length bytes]
```

All multibyte values are little endian. Flags bit 0 is visible; color is 0–15.
Fill rule 0 is nonzero winding, 1 is even-odd. The complete set of contours in
a path is filled together, so holes and overlapping contours follow the
selected rule. Paths paint in the order of the SHOW's reference list like
every other object.

`x,y` translate the asset in panel pixels. Scale is uniform Q8: 256 means 1×,
512 means 2×; valid values are 0–2048 (0×–8×). Scaling acts around the asset's
local origin, then translation is applied, then the object's rotation.

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

Limits, enforced before applying the message:

- 8,192 command bytes and 1,024 commands per path.
- 512 flattened edges per path; subdivision depth at most 8.
- 4,096 edges across the paths on the active list of a SHOW. Each edge occupies
  16 bytes of the asset store as a private asset of its object.
- One definition per object id per message. A message may define several
  distinct path objects atomically.

A new version of a path object replaces it (the previous edges are freed),
stops its animations and resets its rotation to zero. Geometry is immutable
until another version replaces it. Type 18 cannot be drawn by an immediate
mode-36 record.

Paths use the SHOW ops MOVE/GLIDE, VISIBLE, FREEZE and FINISH like other
objects. For TWEEN, only these mask bits are valid:

| Bit | Parameter |
| --- | --- |
| 0 | p0: x translation, signed pixels |
| 1 | p1: y translation, signed pixels |
| 2 | p2: uniform Q8 scale, 0–2048 |
| 8 | color, 0–15 |

Geometry/color tweens keep the existing frame-count semantics. A path's
control points do not morph when a new version replaces it.

## ROTATE (SHOW op 9)

```text
[9:u8][object id:u32][angle:i32][pivot-x:i16][pivot-y:i16]
[duration-ms:u16][curve-x1:u8][curve-y1:u8][curve-x2:u8][curve-y2:u8]
```

This is a 19-byte operation inside a mode-38 SHOW; its target must be on the
SHOW's active list. Angles are **degrees × 256**, positive clockwise.
Allowed targets are -36,000 through +36,000 degrees (100 turns either way).
Angles remain unwrapped during interpolation: 0→360 is one complete turn;
350→10 goes backward 340 degrees. For a short forward turn, send 370 instead
of 10. Exact full-turn angles use the original primitive rasterizer.

The pivot is explicit, in **panel pixels**, and stays fixed when geometry moves.
For a moving-center rotation, update the pivot along with the application's
position targets; for an orbit, leave the pivot fixed. Pivot changes apply
immediately; the pivot itself does not tween. Use the same pivot for every object
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

Supported targets are geometric shapes 1–13 and paths 18. Rotation of an
absent object, image, cached text or inline text refuses the whole message
before any operation in it applies. The all-objects id is not accepted. Text/image
sampling remains axis-aligned. Transformed geometry is generated from the
original coordinates each draw to avoid accumulating rounding error.

## Transaction and lifetime

The entire message's structure, path geometry, target types, reserved fields
and the active edge budget are validated before the cache changes. Staged
definitions are allocated first; failure frees them and retains the previous
objects, background, animations and displayed frame (the reply names the
cause). The scratch workspace can remain cached after a rejected message.

Replacement happens under the display gate. The previous edges are freed only
when the replacement is committed; the display task cannot see an incomplete
upload. Objects that leave the active list stay cached and are evicted least
recently used when room is needed; RESET (mode 41) and mode 11 cleanup free
everything. Scratch workspace is about 10.5 KiB on heap 13, allocated on first
path/rotation use. The cache descriptors use about 38 KiB of heap 13; edges,
images, fonts and strings share the 256 KiB store on the EvenHub heap.

Framebuffer lease expiry/release stops animation, marks the cache lost and
changes its epoch; the memory is released under the display gate afterwards.
Clients learn about it from the STALE reply to their next cache message, reset
the cache and rebuild rather than assuming an old baseline.

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
path object per frame. Representative frames and 24 source-pixel probes per frame
are replayed on the host. The included 300 frames all fit at 144×72.

The dump format extends the old suite replay format: kind 1 + u16 length + raw
message; kind 2 + u32 wait milliseconds; kind 3 + u8 UTF-8 label length + label;
kind 4 + u16 x + u16 y + u8 expected gray; kind 5 requests a PGM snapshot.
Use `vector_host_test OUTPUT_DIR DUMP_FILE` to replay a dump; the old scene
replayer does not understand kinds 4/5.
