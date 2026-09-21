# Retained object cache and fast dashboard reopening

Status: implemented in g2flash as GLASSLYCFW/38 and verified on hardware
(2026-09-20: vector suite 23/23 payloads, shapes suite 43/43, staged demo with a
blank-to-scene reopen of 47 ms and no definitions sent; both lenses acknowledge).
Hardware findings: the right lens processes messages on a task with far less
stack than the left lens's BLE task and reboots on any heap allocation taken
while the display gate is held, so the firmware allocates everything before the
gate and keeps handler state in the heap; its EvenHub heap may not fit 256 KiB,
so the store falls back to 192/128/96/64 KiB and the RESET reply reports the size.
Implementation: `patches/scene.c`
(object cache, modes 37-41), `patches/texture_cache.c` (region-based asset
renderers), `patches/shapes.c` (asset-id records), transport reply kind 5,
host tests in `patches/host/shapes_host_test.c` / `vector_host_test.c`, demos
on `demos/object-cache.ts`. Wire codes and budgets chosen in implementation are
documented at the top of `patches/scene.c`; deviations from this plan are noted
inline below as "Implemented:" paragraphs where they matter.

## Goal

Minimize the time from requesting the CFW dashboard to seeing it on both lenses.
Keep previously drawn objects on the glasses after their view closes, reuse them
on reopening, and evict the least recently used eligible objects when memory is
needed. The phone mirrors cache state and reconciles it while idle. Opening must
not normally wait for a cache query or resend unchanged object definitions.

This is a shared cache for dashboard and miniapp objects, not a reserved dashboard
bank. Cache hits still require on-device rasterization; this plan does not cache
an additional full-panel framebuffer per view.

Backwards compatibility is not a requirement. Replace the existing scene/cache
wire formats and update firmware, both mobile clients, demos, and test tools
together. Maintain one protocol implementation without old-format decoders,
dual encoders, or older-firmware fallbacks.

## Current behavior and constraints

- `patches/scene.h` defines one retained scene with 128 slots. Slot index also
  determines paint order. Text storage is embedded in the scene; paths have
  separate allocations. There is no object identity or residency query.
- `patches/scene.c` can redraw existing slots on COMMIT. CLEAR discards them;
  raster takeover freezes them. Mode 38 subcommand 2 releases the scene.
- `patches/texture_cache.c` exposes phone-owned bytes at raw offsets, not an
  object allocator. The firmware budget is 256 KiB. Glassly's current scene
  texture allocator uses only 64 KiB because shape references use 16-bit offsets.
- Modes 18/19/20 and font-table glyph entries already use 32-bit offsets. The
  remaining narrowing is in scene IMAGE/TEXT/TEXT_CACHED records and the mobile
  scene encoders; increasing the mobile allocator limit alone is insufficient.
- `patches/settings_ext.c` frees textures when the framebuffer lease expires or
  is released. Scene slots can survive that event with unusable texture references.
  Mode 11 cleanup releases both scene and texture storage.
- Each lens has independent firmware state. SID 0xf0 processing acknowledgements
  cover both lenses; the scene-settled notification comes from the right lens.
  Neither is currently a cache inventory report.
- In `../glassly/mobile/modules/engine/src/services/LocalDisplayManager.ts`,
  `setOverlayOwner()` drops the departing overlay's retained phone scene and
  restores main content or clears the panel.
- In the iOS `G2.swift`, `cfwClear()` increments the cache generation. The encoder
  subsequently resets its scene/texture model and repeats scene warm-up.
  Android's clear path differs, but also invalidates placement by clearing the
  last lens boxes. Both implementations need explicit cache-aware behavior.

Some existing comments mention retired texture modes or older memory layouts.
Use current constants and executable code when specifying the new protocol.

## Cache model

### Identity, storage, and presentation

1. Give every logical object a stable, session-namespaced ID and an explicit
   content version. Keep the ID stable across dashboard close/reopen. Distinguish
   pages within the dashboard miniapp so their element IDs cannot collide.
2. Let firmware map identities to reusable physical slots. A recycled slot must
   never satisfy a reference to its former occupant: references include identity
   and version, or a validated generation-bearing handle.
3. Separate the active scene's ordered object references from storage order.
   Switching views changes the active list without deleting inactive objects.
4. Treat image, glyph/font, and path data as tracked dependencies. A resident
   object is drawable only when its entire dependency set is resident and valid.
   Cached-object commands resolve versioned asset references rather than trusting
   phone-selected raw texture offsets.
5. Preserve the last drawn object state on hide. Freeze hidden animations and
   retain their current geometry. Keep mutable animation state distinct from
   immutable content versions; the phone need not predict every timer tick.
   Reopening may atomically apply authoritative geometry/content updates.

### 32-bit asset storage offsets

Implemented: asset-bearing records carry 32-bit asset ids (IMAGE/TEXT 12 bytes,
TEXT_CACHED 16 bytes); firmware resolves ids to store regions through a
16-byte-granule bitmap allocator over the 256 KiB store. Fonts are
self-contained assets (96 relative glyph offsets + glyph images). Inline text
and path commands become private assets owned by their object.

Include removal of the 64 KiB scene addressing limit in this change, using the
new object protocol rather than introducing an intermediate scene record format.

- Use explicit `uint32_t` offsets for asset storage and dependency resolution.
  Keep geometry and animation parameters at their existing widths; do not widen
  every `p[]`, `from[]`, or `to[]` entry merely to carry asset addresses.
- New scene objects reference versioned asset IDs. Firmware resolves those IDs
  to 32-bit storage offsets before drawing. Physical relocation/reuse must not
  change logical identity or allow an old reference to draw a replacement asset.
- Cover image data, cached string data, font tables, and glyph data. Ensure the
  new rendering path reaches the existing 32-bit texture helpers without passing
  resolved addresses through 16-bit shape parameters.
- Make the full existing 256 KiB texture region addressable. This removes the
  phone's 64 KiB restriction without increasing the firmware's current texture
  allocation. Usable payload capacity still depends on allocator overhead and
  fragmentation; descriptor count and other memory budgets remain separate.
- Replace the existing 16-bit asset-bearing scene layouts with the asset-reference
  format. Remove the 64 KiB allocation restriction and obsolete encode/decode paths.
- Update Swift and Kotlin encoding/model code to remove narrowing conversions
  such as `UInt16(offset)` from the new path. Keep wire integer widths explicit;
  a 32-bit asset ID is an identity, not an interchangeable raw storage offset.
- Reuse internal 32-bit upload/draw helpers where they fit allocator ownership.
  Replace externally addressed raw texture writes with allocator-owned asset
  uploads. Upload chunk lengths can stay 16-bit; split larger data across bounded
  transactions.
- Validate ranges without overflow: check `offset <= capacity` and
  `length <= capacity - offset` before access. Audit helper calls, dependency
  tables, cache reports, and phone-side comparisons for truncation or sign errors.

### Bounded allocation and LRU

- Use bounded storage with reusable slots and explicit budgets for descriptors,
  variable-sized data, dependency metadata, active references, and staging.
  Choose exact capacities after measuring current heap headroom; do not assume
  128 cached objects can hold multiple useful views or increase memory blindly.
- Allocate lazily on custom commands. Prefer bounded pools/free lists where
  practical; account for fragmentation and peak replacement memory.
- Pin the displayed scene and its dependency closure. Temporarily protect all
  objects needed by an incoming transaction. Inactive objects are evictable.
- When capacity is needed, evict the least recently used unpinned object, with a
  deterministic tie-break. Reclaim a shared dependency only when no remaining
  resident object references it, or explicitly invalidate all dependents.
- Recency advances on accepted logical uses/updates, not on animation ticks,
  cache queries, or retransmission of an already accepted operation. Touch all
  references in a successfully shown scene using a defined deterministic rule.
- If the working set cannot fit without evicting pinned data, return a capacity
  failure. Do not evict the visible scene to make a failed replacement fit.
- Keep firmware authoritative. The phone predicts allocation/eviction but only
  promotes pending state to confirmed state after the relevant lens acknowledges.

## Protocol design

Implemented wire codes: 37 PUT, 38 SHOW (embedded PUTs, references, ops,
flags PRESENT/KEEP/TAG/FREEZE), 39 HIDE, 40 STATE, 41 CONTROL (reset with a
phone-chosen epoch, drop, period); replies as transport kind 5 with statuses
applied/stale/missing/capacity/refused/unchanged/delta/snapshot/reset. Modes
19/20/36 reference assets by id; mode 18 is retired. See `patches/scene.c`.

Assign wire codes and a new protocol revision during implementation. Replace
existing scene/cache commands, including the old modes 37/38 layouts; retaining
their numeric codes is optional. Update all producers and consumers together.
Revision checking identifies a mismatched installation; it does not select a
compatibility implementation or fall back to an old format.

Proposed logical operations (names are illustrative):

- `PUT_OBJECT`: define/update a versioned object and its dependencies. Support
  bounded staging for multi-message uploads; interrupted uploads are not resident.
- `SHOW_OBJECTS`: ordered references, background, small updates, and request ID.
  Validate the complete scene and dependencies before mutating visible state.
- `HIDE_SCENE`: freeze and remove active references while retaining cached data;
  blank the panel or replace it with another scene without flushing the cache.
- `CACHE_STATE`: request changes since a cache epoch/revision, optionally obtain
  a bounded, paginated inventory snapshot.
- `DROP_OBJECTS` / `RESET_CACHE`: explicit release, with pinned-object handling
  and cache epoch transitions defined.

Replies identify the lens, request, cache epoch, and revision. Distinguish:

- Applied/presented: specify whether an acknowledgement means data accepted,
  frame submitted, or frame actually presented. Preserve existing settle semantics.
- Missing/stale references: return a bounded list or continuation token. Leave
  the current displayed scene unchanged on that lens.
- Capacity exceeded / malformed / stale session: explicit recoverable outcomes.

Use a session handshake or equivalent epoch scheme that cannot confuse a rebooted
cache with an old one. Define counter wrap, duplicate requests, lost replies,
retry behavior, and late responses from superseded views. A retried SHOW with
animation updates must not apply motion twice.

### Open path

1. The phone builds references from the last dashboard scene and current widget
   state. Include only changed data, such as the clock or selection.
2. Send one SHOW transaction without a preceding cache query when residency is
   believed valid. If the phone already knows objects are missing, upload those
   first rather than deliberately taking a miss.
3. On a hit, firmware redraws immediately. No unchanged definitions or texture
   uploads are sent, and no animation warm-up repeats merely because of hiding.
4. On a miss, upload only missing/stale objects and dependencies, then retry.
   Use a complete rebuild for a reset or when selective recovery is impractical.
5. Bound recovery and cancel superseded requests so a late retry cannot reopen
   a dashboard the user has already closed.

Validation is atomic per lens, not automatically across the pair. One lens can
hit while the other misses. Keep separate mirrors, repair the missing side, and
report binocular completion only when both acknowledge the intended scene.
Measure the resulting transient mismatch. Strict simultaneous switching would
need a separate prepare/commit design and may add a round trip; it is not assumed
by this low-latency fast path.

### Idle reconciliation

- Maintain an epoch, revision, and bounded mutation journal per lens. Record
  residency/version changes and enough recency metadata to reconcile LRU order.
- Return a compact unchanged reply when the phone is current, deltas when the
  journal covers the request, and a consistent inventory snapshot otherwise.
  Pin snapshot revision across pages or reject/restart an inconsistent snapshot.
- Include current revision in ordinary replies where feasible. Commit phone
  bookkeeping in operation order; reject stale or out-of-order query responses.
- Schedule low-priority queries only when display/transport work is idle, with
  throttling and backoff. Query on reconnect and uncertain delivery as well.
  Do not wake an otherwise sleeping device solely to poll the cache.
- Queries neither update LRU order nor guarantee a future hit. SHOW must always
  validate references even after a successful background reconciliation.

## Lifecycle and protocol transition

- Normal dashboard hide retains objects. Restoring another app makes the old
  dashboard objects eligible for eviction rather than deleting them immediately.
- Initially keep existing lease/cleanup memory-release behavior: lease loss,
  mode 11, and reboot invalidate relevant cache state explicitly. Do not extend
  ownership or retention across sleep/lease loss as an incidental optimization.
- Track texture/dependency loss even if descriptors remain allocated; never claim
  those objects are drawable. A whole-cache epoch reset is an acceptable first
  implementation for uncertain lifecycle events.
- App stop, replacement, changed settings, and account/session changes must
  invalidate phone replay eligibility independently of physical cache residency.
- Require the matching scene/cache protocol revision on the phone and glasses.
  Remove older-firmware scene/cache fallbacks and obsolete format handling.
- Keep changes in C extension code and phone code where possible. Avoid new
  firmware offsets, boot-time hooks, and allocations before custom messages.

## Implementation sequence

### 1. Measure and finalize protocol

- Instrument open request, view grant, scene preparation, native encode, bytes
  sent, BLE queue/transfer, each lens's processing acknowledgement, and presentation.
- Establish cold-open and warm-reopen baselines, including reopen after another
  miniapp has drawn. Separate phone/view-grant time from BLE and rasterization.
- Specify exact identities, wire formats, budgets, LRU tie-break, dependencies,
  duplicate handling, lifecycle transitions, and stereo recovery behavior.

### 2. Implement firmware cache and host tests

- Replace the current scene/cache storage with bounded object/dependency storage
  and LRU bookkeeping. Reuse the current rasterizer and owned panel shadow.
- Add an active ordered reference list, pinning, and staged transactional changes.
- Implement 32-bit asset offset resolution through the raster path and enforce
  allocator ownership for asset uploads. Delete superseded record decoders.
- Add cache state replies/journal and lifecycle invalidation under the existing
  serialization rules. Validate thread safety with lease expiry and scene timers.
- Migrate affected demos, payload generators, replay tools, and test fixtures to
  the replacement protocol; remove tests whose only purpose is old-format support.

### 3. Implement native phone mirrors

- Update iOS and Android `G2CfwScene`, `G2CfwRenderer`, `G2CfwTextureCache`, and
  `G2` transport/lifecycle integration in `../glassly`.
- Track confirmed and in-flight residency separately per lens. Retain IDs and
  versions across view switches; support selective uploads and bounded recovery.
- Use the advertised full texture capacity and encode asset references without
  16-bit narrowing. Delete superseded encoders and the 64 KiB fallback.
- Separate panel contents, scene residency, texture residency, and timer warm-up
  state. Blanking the panel must not imply all four were reset.
- Add background reconciliation without delaying active display work.

### 4. Integrate dashboard reopening

- Update `SceneRenderer` / `LocalDisplayManager` to retain a hidden overlay's
  replay source while continuing to enforce view ownership and stale-work guards.
- Update dashboard `PageHost`, `DashboardPage`, and element identity generation
  as needed. Preserve the view-grant contract; measure before optimizing that path.
- Recompute current clock, battery, calendar, selection, and eligibility on open;
  do not flash stale cached values before applying known changes.
- Reuse unchanged objects, update changed ones, and force a presentation after
  blanking even when all object definitions compare equal.

### 5. Validate on hardware and document

- Run applicable existing host/raster/scene/transport tests and firmware build
  checks, plus new cache tests, before hardware evaluation.
- Test the coordinated protocol update on both mobile platforms. Use a controlled hardware
  rollout with the existing recovery/OTA path intact; this plan does not authorize
  flashing or release.
- Document the final protocol, memory limits, lifecycle, and measured latency.

## Required tests and acceptance criteria

- Deterministic LRU: hide retains; use refreshes recency; queries do not; pinned
  objects survive pressure; ties and retries produce predictable results.
- Identity safety: recycled slots, stale versions, namespace collisions, reboot
  epochs, counter wrap, and delayed replies cannot draw the wrong object.
- Dependencies: shared textures, partial uploads, texture loss, allocator
  exhaustion/fragmentation, path budgets, and replacement peak memory are handled.
- Offset boundaries: render images, cached strings, and font tables/glyphs below,
  at, and above `0xFFFF`/`0x10000`, including data spanning the boundary. Verify
  valid allocations ending exactly at 256 KiB and reject out-of-range or wrapping
  offsets/lengths, including `UINT32_MAX`. Cover Swift/Kotlin wire parity and the
  firmware decoder. Reallocation above 64 KiB cannot revive stale asset IDs.
- Protocol transition: firmware, Swift/Kotlin encoders, demos, and replay tools
  agree on the replacement format. Revision mismatches are reported explicitly;
  no old-format fallback or externally addressed texture-write path remains.
- Transactions: a failed update/SHOW leaves the previous visible scene valid;
  staged objects are reclaimed after cancellation; rejected work cannot corrupt
  pinned dependencies. Inject allocation failure at each staging step.
- Rendering: paint order is independent of allocation order; hidden animation
  freezes; reordering, text, images, paths, and resumed/updated geometry are correct.
- Synchronization: dropped ACKs, duplicate messages, stale inventories, journal
  overflow, concurrent cache changes, one-lens reset, and asymmetric misses recover.
- Lifecycle: lease expiry, cleanup, reconnect, app stop, dashboard disable, rapid
  open-close-open, and switching between dashboard pages cannot replay stale work.
- Warm unchanged reopen sends no object definitions, asset uploads, residency
  preflight query, or repeated warm-up; one logical SHOW is sufficient on a hit.
- A partial miss retransmits only required missing/stale data; a known reset
  rebuilds deterministically. Background traffic yields to dashboard opening.
- Report p50/p95 request-to-visible latency, bytes, logical messages, cache hit
  rate, and worst-case memory for cold open, blank-to-dashboard reopen,
  app-to-dashboard reopen, cache pressure, and reconnect. Confirm actual display
  timing on hardware rather than equating processing ACK time with photons.
- Set numerical latency and memory targets from the baseline before rollout;
  do not claim a millisecond improvement from source inspection alone.
