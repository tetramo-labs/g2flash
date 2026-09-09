# Revision 23: combined graphics and upstream sensors

This revision merges upstream main through `1507192` with the revision-21
graphics/SVG/rotation and revision-20 ANCS extensions. All existing capability
tokens remain in the 151-byte `EVENCFW/23` string. Check revision >= 23 for the
combined sensor contract; do not require upstream's `ringbat17` token.
Adding that token and the battery field to the settings response would exceed
the size already known to break settings reads (see commit `8746d55`). The
settings response still includes the full microphone status. On the master lens,
a successful response is followed by a separate battery notification.
Clients that gate ring support only on `ringbat17` must also accept revision 23
or discover it by the `RB` report.

## Shared image-handler modes

| Packet | Meaning |
| --- | --- |
| `[16,count,records...]` with complete records | Existing immediate shapes, including inline text; unchanged inside mode-8 batches |
| `[16,0]` | ALS query; formerly an empty shape list, still writes no shadow pixels, now skips display presentation |
| `[16,1,...]`, 2–9 bytes | ALS passive start, including upstream's omitted-parameter defaults |
| `[16,2]` | ALS passive stop |
| `[17,0]`, exactly 2 bytes | Read-only ring-battery query |
| `[17,flags,bg,ops...]`, at least 3 bytes | Existing retained scene, including compiled paths and rotation |
| `[18,op,...]` | Existing animation control |
| `[19,op,...]` | ALS alias using the same payload as upstream mode 16; preferred for new clients |

ALS packets do not acquire the display gate. Complete shape records and scenes
still do. A one-record inline-text shape needs at least 15 bytes including the
mode/count header, so it cannot be confused with a short ALS command. Empty
scene updates always have their background byte, so they cannot become battery
queries. Mode 11 still releases graphics, texture, microphone, ANCS, compass,
and passive ALS session resources.

## Settings channel fields

The field number alone is insufficient to distinguish the two sensor/ANCS
extensions; check direction and the versioned body prefix before decoding.

| Direction / field | Body prefix | Meaning |
| --- | --- | --- |
| Glasses → phone, 105 | `AN`, version 1 | Existing ANCS relay records and status |
| Glasses → phone, 105 | `AL`, version 1 | 24-byte ambient-light report (see `als_sensor.c`) |
| Phone → glasses, 106 | `AN`, version 1 | Existing ANCS control |
| Glasses → phone, 106 | `RB`, version 1 | 5-byte ring-battery report (see `../docs/ring-battery.md`) |

## Compass

Mode 10 preserves `[10,0]` stop and `[10,1]` stock-default start, and adds
`[10,2,interval:u16LE,min-change:u16LE]`. The interval is clamped to 50–2000 ms.
The new sensor-hub hooks preserve the stock UI event and forward the heading
with sample-matched diagnostics on sid 8, field 100 (`CM`, version 1).
The former CFW display-forwarding hook is removed so it cannot duplicate the
new notification. Stock navigation and ANCS hooks remain installed.

## Offline validation

Run `python3 patches/host/run_vector_tests.py --out /tmp/g2-vector-tests` for
sanitized ANCS, routing, settings-size, ring-cache, compass, shape/scene, SVG,
rotation, TypeScript, wire-fixture, video-replay, and ARM compilation checks.
`./build_cfw.sh --skip-venv` regenerates the patch set for comparison, applies
the committed JSON to stock firmware, and verifies the pinned output hash.
These checks do not exercise physical sensors or BLE on glasses.
