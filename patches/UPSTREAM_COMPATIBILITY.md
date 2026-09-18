# Revision 24: separate local packet numbers

Local extension numbers are increased by 20. Upstream sensor numbers are
unchanged. There are no length-based overloads or aliases.

| Extension | Old | Revision 24 |
| --- | --- | --- |
| Immediate shapes | Mode 16, `shapes16` | Mode 36, `shapes36` |
| Retained scenes, SVG paths and rotation | Mode 17, `scene17` | Mode 37, `scene37` |
| Animation control | Mode 18, `anim18` | Mode 38, `anim38` |
| ANCS reports/status, glasses → phone | Settings field 105 | Settings field 125 |
| ANCS control, phone → glasses | Settings field 106 | Settings field 126 |

Only the outer packet/field numbers and capability tokens change. Shape type
numbers (including inline text type 17), scene operation numbers (including
path op 8 and rotation op 9), record formats and ANCS `AN` version-1 bodies
remain unchanged. Protobuf bytes tags for ANCS are now `EA 07` (field 125)
and `F2 07` (field 126).

Clients must migrate to the new numbers and check `GLASSLYCFW/24` or later. All
repository demos and host replay consumers use the new numbers. External clients
such as Glassly must update their encoders and ANCS listeners too; old packet
numbers are not graphics/ANCS aliases.

Upstream keeps mode 16 for ALS, mode 17 for ring battery, settings field 105
for `AL` reports, and field 106 for `RB` reports. Mode 19 is no longer an ALS
alias. Mode 10 compass controls/diagnostics and mode 11 session cleanup remain.

# Revision 26: BLE link speed control

Revision 27 adds one glasses → phone field: settings field 129, body
`[tagLo][tagHi]`, sent by the master lens when a scene commit tagged with
mode-37 op 10 has settled. Upstream has no equivalent; stock decoders skip it.

Upstream forces its fast BLE profile in flash (7.5 ms interval, every
connection-parameter request turned into "fast"). Revision 26 keeps the LE 2M
feature bit static but gates the two other effects on a context flag that the
phone sets through sid-0x09 field 127, body `['B','L',1,op]` with op 0 = stock,
1 = fast, 2 = query. The default is stock behaviour. Each lens has its own link,
so send the control to both. Every settings READ reply carries field 128:
`['B','L',1, fast, side, wantedMode, appliedMode, min16, max16, latency16,
liveInterval16, liveLatency16]` (interval units 1.25 ms; modes 0xa3 fast, 0xa4
slow, 0 = update pending; the live pair is the connection as the central
granted it). Switching queues a connection-parameter request immediately
through the stock state machine; mode 11 cleanup switches back to stock. The
two forced in-place edits became `bl` retargets at `0x47ae52` and `0x47b444`,
and a third at `0x47b20e` gates the stock "already fast" test: stock skips the
request for any link under 31.25 ms with zero latency, which a phone's or Mac's
default 15-30 ms link satisfies, so without it the 7.5 ms profile was never
requested (confirmed on hardware: the flag set, the request queued, the link
unchanged). See `patches/ble_link.c` for the recovered stock mechanism. The
settings reply is now 104 bytes. The hooks are hardware-unverified beyond that
observation.

# Revision 25: numeric revision only

Following upstream's move from capability tokens to `Faceclaw/<n>`, field 100
now carries just `GLASSLYCFW/25`. Feature tokens are gone; clients gate on the
revision number (`demos/glassly-cfw.ts` shows the parse). The revision-24
feature set is unchanged. Revision 25 also picks up upstream's fast BLE
profile: LE 2M is enabled in the local feature set, the fast connection
profile requests a 7.5 ms interval with latency 0, and the delayed slow-mode
request is forced back to fast, so the stock 60-second slow-mode timer no
longer throttles image traffic. The phone still has to request 2M PHY and
agree to the interval.

With the 44-byte stock reply and full microphone status the settings response
is now 84 bytes. Ring-battery status still follows in a separate notification
(the single-frame size pressure that motivated it is gone, but the phone-side
decoder expects the separate report). Detect ring support by revision 24 or
later, or by the `RB` report.

Run `python3 patches/host/run_vector_tests.py --out /tmp/g2-vector-tests` for
sanitized C tests, TypeScript fixtures, video replay, and ARM compilation.
`./build_cfw.sh --skip-venv` checks patch reproducibility and the image hash.
Physical sensors and BLE still require device validation.

## Revision 30: upstream merge (Faceclaw/4 → Faceclaw/14)

Upstream now uses modes **18/19/20** for the 256 KiB texture cache (uint32
offsets) and private SID **`0xf0`** for its message transport; both are taken
as-is, so the local numbers above (36/37/38, fields 125–129) remain free of
collisions. Unlike upstream, modes 12/13/14 (uint16 offsets) are still
accepted, and the stock EvenHub image path is still patched, so clients on
either transport keep working. On the SID-`0xf0` path modes 3/6 carry plain
RLE (the transport inflates); on the stock image path they still carry
`zlib(rle)`.

## Revision 31: SID-`0xf0` only

The stock EvenHub image path is no longer patched, so mode-prefixed payloads
sent as image-container updates are handled by the stock loader (and rejected
as non-BMP). Modes 12/13/14 are retired; TEXT_CACHED shape records now need a
mode-20 font (96 uint32 glyph offsets). Everything else (modes 3/6/8/9/10/11/
15/16/17/18/19/20/36/37/38, fields 105/106/125-129) is unchanged on the wire,
with modes 3/6 carrying plain RLE because the transport inflates.
