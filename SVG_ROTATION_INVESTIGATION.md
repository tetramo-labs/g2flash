# SVG, Bad Apple, and animated rotation

Investigated 2026-09-05 against the local g2flash and `/Users/fosse/dev/glassly` checkouts. This is an implementation proposal, not an implemented feature. No firmware was flashed.

The practical architecture is SVG import/compilation in Glassly, compact retained paths in the firmware, and a shared transform animation model for paths and existing geometric shapes. Implement rotation first, then filled paths, then a fourth Bad Apple playback mode. Arbitrary path morphing is a separate feature.

## Existing foundation

| Layer | Current behavior | Relevant source |
| --- | --- | --- |
| Firmware drawing | 640×480 packed 4bpp; integer rasterizer; lines, rectangles, circles, triangles, quads, stroked quadratic/cubic curves, arcs, pies, images, text | `patches/shapes.h`, `patches/shapes.c` |
| Retained scene | 128 slots, painted in slot order; full scene clear/redraw per presentation | `patches/scene.h`, `patches/scene.c` |
| Animation | One animation state per slot; eight geometry parameters plus color/width; cubic-bezier easing; default 33 ms period | `patches/scene.c` |
| Glassly API | Stable element IDs and `transition: {durationMs, easing}` already reach native encoders | `mobile/modules/miniapp/src/modules/display.ts` |
| Native encoding | Elements become slots; changes become SET or TWEEN; overflowing scenes fall back to raster rendering | iOS and Android `sgcs/G2CfwScene.*`, `sgcs/G2.*` |
| Mirror | Skia geometry and Reanimated transitions | `mobile/src/components/mirror/sceneTween.ts` |
| Bad Apple | Text, bitmap, or up to 80 tracked filled rectangles; 300 stored 156×80 frames at 10 fps | `miniapps/example-miniapp/src/background/BadApplePlayer.ts`, `badAppleShapes.ts`, `badAppleFrames.ts` |

Paths do not exist in the public scene type or firmware record grammar. A cubic curve currently occupies all eight geometry parameters and is only stroked. The polygon filler accepts at most eight vertices and has no general compound-path interface. Its even-odd scanline approach is useful groundwork, but increasing that array alone would not implement SVG.

The current scene allocation is about 24 KiB for slots and inline text, plus a lazily allocated 150 KiB framebuffer. Textures use a separate 64 KiB cache. Frame allocation failure permits static rendering but refuses animation. New path storage must have a measured, bounded memory budget and respect framebuffer lease teardown.

The existing local `g2_2.2.9.22_cfw.bin` leaves 143,000 bytes below the patcher's conservative MRAM ceiling at `0x007f0000`. This is code/storage headroom for that binary, not free heap or a measurement of a newly rebuilt source tree. The injected code is freestanding Thumb-2 and the build rejects unresolved external calls/unsupported relocations.

## Rotation with duration

Suggested public API, reusing the existing transition contract:

```ts
// First render establishes the object; a later render with this same ID
// and a changed angle starts the transition on the glasses.
await session.display.render([{
  type: "rect",
  id: "card",
  box: {x: 200, y: 140, w: 120, h: 80},
  style: {fill: true},
  rotationDeg: 90,
  pivot: {x: 0.5, y: 0.5},
  transition: {durationMs: 800, easing: "ease-in-out"},
}])
```

Proposed semantics: positive angles are clockwise, the default pivot is the unrotated box center, and explicit pivot coordinates are normalized within that box. Point shapes derive their unrotated box from their points. Anchoring is resolved before rotation; the whole element, including any fill/stroke slots, uses the same pivot.

Keep target angles unwrapped: 0→360 must perform a full turn. Thus 350→10 means -340 degrees; a shortest-turn convenience could translate that target to 370 before encoding. Normalize only for trigonometric lookup. Preserve fractional angles, ideally with an explicit fixed-point wire representation, and match that quantization in the mirror.

Firmware work:

1. Add explicit transform state per slot, including current/start/target angle and pivot. Preserve the existing record format and introduce a capability-gated transform operation with a specified field representation. There are no spare universal geometry parameters: quads and cubic curves use all eight, and the existing TWEEN validator only accepts bits 0–9.
2. Tween the angle and transform original geometry at draw time. Do not overwrite vertices with their rotated positions each tick: repeated rounding drifts. Do not interpolate pre-rotated endpoints: a 180-degree turn can collapse a polygon halfway through.
3. Reuse the Q15 sine/cosine table already used for arcs, extending lookup interpolation for sub-degree angles. Use checked/wider intermediates before final clipping. Existing geometry clamps cannot simply run before a transform because an offscreen vertex can rotate onscreen.
4. Lines, triangles, quads, and Beziers transform their vertices/control points. Plain rectangles become quads. Rounded rectangles need a transformed contour; circles retain their radius but their center may orbit an external pivot. Arcs/pies transform centers and add the rotation to their angles.
5. Geometry and rotation must share consistent retarget/freeze/finish behavior. Today there is only one tween per slot. Updating one property must preserve the intended targets for other properties, including during an in-flight move.

The existing duration implementation rounds milliseconds to 2–255 frames at 33 ms: approximately 66 ms–8.415 s. Animation advances one step per successful timer tick, so contention can extend wall-clock duration. Rotation can ship with these documented limits. For accurate/longer durations, add an elapsed-time-based operation with a wider millisecond duration and update native/mirror completion semantics together.

Image and text rotation needs additional transformed texture sampling. Their current blitters are axis-aligned; wrapping only the geometry rasterizer will not rotate them. Scope the first release to geometric shapes and paths, and expose that distinction through capabilities. Whole-SVG rotation should cover every constituent path with one shared pivot.

## SVG support

Expose an SVG element/import helper in Glassly and compile it to an internal path asset. The firmware retains and rasterizes vector geometry; it does not need XML parsing during drawing. A precompiled asset form should be available for bundled demos so playback does no XML parsing.

Recommended first SVG subset:

- `viewBox` and aspect-ratio handling, nested static transforms, basic geometric elements, and multiple paths/subpaths.
- Normalize path commands to absolute M/L/Q/C/Z. Resolve relative/shorthand commands and convert elliptical A commands to cubics in Glassly.
- Solid fills in the panel's 0–15 intensity range. Support both even-odd and nonzero winding fills, or explicitly reject nonzero assets until that rule is implemented. SVG defaults to nonzero; silently treating everything as even-odd is incorrect. [SVG painting specification](https://www.w3.org/TR/SVG/painting.html#FillRuleProperty).
- Prioritize fills for Bad Apple. Add a documented solid-stroke subset afterward; joins, caps, dashes, opacity, gradients, masks, filters, embedded images, fonts, and embedded SVG animation each need an explicit support/fallback policy.

The core firmware additions are a bounded path command decoder, adaptive curve flattening with a screen-space tolerance and segment cap, and an active-edge scanline filler supporting multiple contours and holes. Existing pixel/span routines and the 4bpp frame can be reused. Contours within one compound path must be filled together: filling each separately destroys holes. Antialiasing is an additional quality/performance decision; the current geometry rasterizer is center-sampled.

Use one retained slot per painted compound path, not one slot per segment or triangle. An SVG can still occupy multiple slots when it has multiple independently painted paths. A SVG element must keep stable internal path ordering and share transforms across these slots; replacing its asset need not imply morphing it.

Add bounded path assets addressed by ID/generation, with upload, validation, reference, and release semantics. This can reuse the existing transport and cache-upload pattern, but should not blindly share writable texture-cache bytes. Uploading new geometry over a live asset could change what an animation tick draws before the new scene commits. Stage a new asset/version, validate it completely, switch references atomically at COMMIT, and retire the previous version after no scene uses it. Two reusable staging regions are sufficient for a basic streaming demo; budget them together with the active scene, edge workspace, textures, and framebuffer.

Do not commit to fixed asset limits before measuring real traces. Enforce limits on bytes, commands, flattened edges, contours, and total work per frame. An oversized asset must fail before it changes the active scene, with an explicit fallback or degraded result. Reconnects and dropped queued frames must invalidate/rebuild the asset baseline along with slot state.

NanoSVG is a possible normalization reference: its parser emits cubic Bezier shapes and applies viewBox transforms. It is not a drop-in firmware renderer: it depends on C library/math facilities, and its repository says it is not actively maintained. A parser/library decision still needs corpus and runtime validation. [NanoSVG project](https://github.com/memononen/nanosvg).

## Bad Apple SVG demo

Add an `svg` mode beside the existing three modes, using the existing clock and at-most-two-in-flight presentation control.

1. Extend the offline asset generator to trace video frames into compound filled contours. Potrace is a suitable tool to evaluate: it converts bitmaps to scalable contours and can produce SVG. Sweep simplification settings while preserving visible holes and small features. [Potrace documentation](https://potrace.sourceforge.net/potrace.1.html).
2. Generate both reviewable SVG frames and compact binary path assets with frame metadata, bounds, and complexity statistics. Use the current 30-second clip for the first test; a full-song demo needs a longer source and new generated assets. The current source GIF is 288×144 and the bundled data is further reduced to 156×80, so it cannot establish final high-resolution quality.
3. Start with atomic frame replacement at 10 fps. Keep video timing on the phone and skip late frames. Retain only the current/next vector frame on glasses; do not store the whole video in firmware RAM or the patch blob.
4. Add a separate SVG transform demo to demonstrate firmware-local move/rotate/scale/intensity transitions. A video made of SVG frames and a continuously morphing path are different capabilities.

Smooth path morphing requires matching contour identities, winding, start points, segment counts, and corresponding control points. Independently traced video frames routinely split/merge contours or introduce/remove holes. Matching command structures is also the prerequisite for SVG's own ordinary path interpolation. Use explicit frame cuts where topology changes; treat topology normalization/morphing as a later project. [SVG path interpolation rules](https://www.w3.org/TR/SVG/paths.html).

I ran `bun bad-apple-tests.ts --dry-run` over all 300 frames:

| Existing mode | Average encoded payload/frame | Total payload |
| --- | ---: | ---: |
| Half-block text through the harness raster path | 167 B | 49 KiB |
| Compressed bitmap | 299 B | 88 KiB |
| Retained rectangles | 689 B | 202 KiB |

These are the standalone harness's payload lengths, not BLE packet sizes, radio throughput, or measured FPS. Its reported 10 fps is configured timing in dry-run mode; it does not exercise hardware. BLE/protobuf framing, per-lens transmission, and ACK timing are excluded. The harness has its own encoders and is not byte-for-byte evidence of current Swift/Kotlin output; its bridge-size estimate for rectangles is only an approximation. SVG payload sizes have not yet been measured.

This baseline makes it unsafe to assume vectors save bandwidth. Their immediate value here is contour quality and reusable transformable assets. Compare traced paths against compressed raster at equal visual quality and measure median/p95/max frame bytes and actual device render/presentation times before selecting a target beyond 10 fps.

## Glassly implementation map

Paths below are relative to `/Users/fosse/dev/glassly`.

| Concern | Files |
| --- | --- |
| Public Miniapp SDK types and API | `mobile/modules/miniapp/src/modules/display.ts`, `mobile/modules/miniapp/src/session.ts`; documentation in `sdk/docs/display.md` |
| Input validation, transforms, bounds, hashes, diff propagation | `mobile/modules/engine/src/utils/display/scene/{types,process,differ,anchor,degrade}.ts` |
| Capability negotiation and duration/presentation handling | `mobile/modules/engine/src/services/{effectiveCapabilities,SceneRenderer}.ts`; native CFW classification/status publication |
| Native bridge decoding | `mobile/modules/bluetooth-sdk/ios/Source/DeviceManager.swift`, Android `DeviceManager.kt` |
| Native frame models and copying helpers | iOS/Android `sgcs/SGCManager.*` |
| Path uploads, transform encoding, asset generations, fallback | iOS/Android `sgcs/G2CfwScene.*`, `G2CfwRenderer.*`, `G2.*`; reuse patterns from `G2CfwTextureCache.*` |
| Phone mirror | `mobile/src/components/mirror/sceneTween.ts`, `GlassesDisplayMirror.tsx` |
| Demo player, generation, UI/RPC | `miniapps/example-miniapp/src/background/BadApplePlayer.ts`, `scripts/generate-bad-apple.ts`, `src/shared/channels.ts`, `src/background/controllers/TesterController.ts`, `src/ui/pages/tester/DisplayPage.tsx` |

Bounds need particular attention: `process.ts` currently clips axis-aligned boxes before native encoding. That must not resize an SVG's logical viewport or move a rotation pivot. Preserve original geometry/viewport separately from the visible transformed bounds. Likewise, add transform changes to content identity and copying helpers so an unchanged box does not suppress an angle-only update.

Advertise paths and geometric rotation independently from the existing broad `animation` boolean. Old firmware rejects unknown TWEEN bits/opcodes; gate new encoding on an explicit protocol revision/capability. The firmware capability string is constrained to one BLE frame, so account for its existing size. Update both platforms: current iOS explicitly prefers raster for non-transition image-only scenes; the inspected Android enqueue path attempts vector encoding first, so fallback behavior already differs.

## Delivery and validation

Planning estimates for one engineer familiar with both repositories, including both native platforms and tests; these are not measured schedules:

| Increment | Approximate effort |
| --- | --- |
| Geometric rotation, shared pivots, existing bounded duration API, native/mirror parity | 3–5 engineering days |
| Filled compound paths, SVG normalization, bounded asset lifecycle, native/mirror support | 5–10 additional days |
| Offline tracing, fourth Bad Apple mode, device profiling and tuning | 2–4 additional days |
| Arbitrary morphing, full SVG features, rotated image/text sampling | Separate scope; estimate after prototypes |

The existing host rasterizer/scene test passed with zero failures (compiler emitted unused-function warnings). No phone builds or hardware performance tests were run because this investigation changes no implementation.

Implementation acceptance should include 0/90/180/360-degree turns without shrinking; shared/external pivots; retargeting movement plus rotation; freeze/finish and duration limits; transformed offscreen geometry; SVG holes and winding; malformed/oversized assets preserving the old scene; failed uploads/reconnects; resource release with the lease; and Swift/Kotlin byte fixtures agreeing with firmware decoding. Render representative traced frames on the host for visual comparison, then measure both lenses on hardware. Rebuild the patch JSON/image and pass the existing MRAM/checksum gates before any hardware installation.
