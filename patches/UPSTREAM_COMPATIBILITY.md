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
