# Ring touch-down hardware test

Run from `demos`:

```sh
bun ring-touch-test.ts --timeout 120 --count 3
```

The Mac Bluetooth controller must be on and both lenses available for a direct
connection. Leave the ring paired to the glasses: the test receives its events
through the glasses, without opening a competing ring connection. Wait for
`READY`, then touch, hold briefly, and release three times.

The test opens an EvenHub page, sends framebuffer lease control to both lenses,
keeps the page and leases alive, and logs input events with their protobuf bytes.
Success requires three SysEvent 14 events. Where the private `RI` extension is
present, it must identify raw ring wire type 10; repeated ring ticks do not count
twice. Older firmware can send ring-specific event 14 with source unspecified.
The test releases both leases and closes the page and BLE session on completion
or Ctrl-C. It makes no firmware changes.

## 2026-09-21 live result

Both lenses reported stock version `2.3.0.24`; the right settings response
identified `GLASSLYCFW/38`. From approximately 21:31:56 to 21:33:35 UTC, the
listener received **82 input events and zero touch-down events**. After repeated
taps, swipes, and holds reproduced the problem, the operator stopped the test
early with SIGINT (exit 1, `INTERRUPTED: 0/3`), and cleanup disconnected both
lenses. This was an observed negative result, not a completed 180-second timeout.

Representative received reports (all source 2, ring):

| Gesture | SysEvent | RI wire type | Ring tick |
| --- | --- | --- | --- |
| Tap | 0 | 1 | 7830992 |
| Swipe up | 1 | 4 | 7835046 |
| Swipe down | 2 | 5 | 7835651 |
| Long press | 9 | 0 | 7849719 |
| Release | 10 | 8 | 7850650 |

Long-press and release protobuf payloads:

```text
08026a151a1308091002a2060c5249010100000000f7c67700
08026a151a13080a1002a2060c52490101080000009aca7700
```

No SysEvent 14 or RI wire type 10 appeared. Receiving the private `RI` reports
demonstrates that the firmware's lease-gated ring forwarding path was active.
The issue therefore reproduces without the Glassly phone app or example
miniapp in the path. This does not establish whether the ring fails to produce
finger-down reports or whether a report is lost before this forwarding point.

Source inspection in the sibling Glassly checkout found event 14 mapped to
`touch_down` in both native G2 SDKs, exempted from click debounce, and handled
by the example miniapp's InputPage history and touch indicator. No change was
made to those files. The next investigation should inspect ring report
generation/configuration and the glasses receiver before changing miniapp
event routing. `faceclaw_ring_report` currently captures only the exact
11-byte `00 09 61 00 ...` family; this test cannot see other ring packet families
that the glasses do not forward.
