# g2flash

`g2flash` is a utility for installing firmware on Even Realities G2 smart
glasses, as well as a collection of firmware modifications that add features and
fix limitations of the glasses. The modifications themselves are made with
[Faceclaw](https://github.com/jimrandomh/faceclaw) in mind, but can be used by
any software that communicates with the G2 glasses using their BLE protocol. If
you are using this with Faceclaw, you can either use this tool to apply the
firmware mods, or use the flashing tool built in to Faceclaw's onboarding
process (both versions install the same firmware image). Installing using
Faceclaw is a bit more user friendly than installing using g2flash.

This repository contains patches applied to firmware, but does not contain the
Even Realities firmware itself. The `build_cfw.sh` script will download the base
firmware from Even's CDN, apply patches, and verify that the resulting firmware
has the expected hash for you.

Flashing a custom firmware will void your warranty. This tool will require you
to acknowledge that you are voiding your warranty when you run it.

DEVELOPING YOUR OWN FIRMWARE MODS IS MUCH RISKIER THAN INSTALLING FIRMWARE THAT
HAS ALREADY BEEN TESTED. If you are considering writing firmware mods and you
can complete a project using only phone-side changes, you are almost certainly
better off doing it that way. It is a bad idea to work in this codebase using a
non-frontier language model, or without a proper coding harness, or a
nontechnical user steering.

Changes that involve overwriting parts of the existing firmware are more risky
than changes to the existing C extension files. Changes that run or affect
behavior prior to receiving any custom messages are especially risky, because
if you make the glasses crash on boot, they won't be able to accept an OTA
update. Rebasing the mods onto a new base firmware is an especially hazardous
operation, since it requires updating every firmware offset correctly and not
missing any.


## Modifications

Custom messages reach the glasses over **private SID `0xf0` message streams**
(merged from upstream `Faceclaw/4` through `Faceclaw/14`); since revision 31
the stock EvenHub image path is no longer patched at all, so no image
container is needed and no mode-prefixed payload is accepted there. A stream
of five-byte-header records (`[flags][len:u16][crc16]` + body) is
reconstructed across arbitrary packet boundaries before any TPL reassembly,
on each lens the options byte selects (the other lens receives it over the
frame bridge, in arrival order), and a processing ACK or NACK returns through
the BLE ingress lens with a three-entry history so a lost reply never
re-executes a command. Compression is done at the transport level (a
persistent inflater per ingress lens, reset with `RESET_CONTEXT`), the decoded
CRC-16 is checked, messages can be up to 65,535 bytes and need no EvenHub
layout; modes 3/6 carry plain RLE. All commands dispatch through one owned
640x480 packed-4bpp shadow under one mutex. `python3 send_message_probe.py
--dry-run` shows the packet splitting; run it with your usual connection URL
to exercise the path (the default payload is a mode-7 no-op and the debug
overlay shows the last received size and CRC).

Mode 11 cleanup releases the shadow, the texture cache and the scene once
pending display refreshes have finished.

Image traffic is compressed with
zlib+RLE. Screen contents can be up to 640x480 (larger than the screen area
supported by the stock firmware), you can update dirty rects rather than
updating the whole screen at once, and you can send messages which perform
rect-to-rect copies for low-bandwidth scroll animations. A changed-region
update refreshes only the panel rows it touched over QSPI rather than pushing
the whole 640x480 panel each frame, and the zlib inflate stream and its 32 KiB
window are kept alive for the whole session (reset per frame) instead of being
allocated and freed on every update. Because this mode
writes directly to the framebuffer without going through EvenHub's
screen-update functions, stock containers do not contribute visible content
while the direct framebuffer lease is held. A lease-scoped 256 KiB texture cache lets the phone upload RLE
icons and glyphs once, then draw cached images and strings with small update
messages. The cache is allocated on the EvenHub heap (falling back to heap 13)
and zeroed on its first write, and released when the Faceclaw framebuffer lease
ends. Modes 18/19/20 (upstream `Faceclaw/13`) take 32-bit cache offsets,
including glyph-table entries, for the full 256 KiB; the 16-bit modes 12/13/14
were retired in revision 31. Upload lengths remain 16-bit. Cached draw commands carry an options
byte whose low nibble selects the top output color; bit 4 makes source color 0
transparent, and bit 5 reverses the proportional 16-entry color ramp.
Image-handler mode 15 draws a length-prefixed UTF-8 string with the glasses'
built-in 20 px font chain and its default pair kerning. Its payload after the
mode byte is `[x:u16][y:u16][options:u8][strlen:u8][UTF-8 bytes]`; options match
the cached draw commands, and inline bytes 1–31 adjust x by -10 through 20 just
as they do in cached-font mode 20.

The Glassly build of this firmware (branch `glassly-cfw`, base 2.2.9.22) adds
vector shapes and firmware-side animation on top of the texture cache:

 * Mode 36 draws a list of 20-byte shape records straight into the shadow:
   `[36][count][records]`. A record is
   `[type][flags][color][width][p0..p7 as int16 LE]` and covers hairline and
   wide lines, plain and rounded rectangles (fill or stroke), circles and rings,
   triangles, quads, quadratic and cubic beziers, arcs and pie sectors, plus
   cached images and text (built-in 20 px font with the string carried inline in
   the record and clipped to its box, or read from the texture cache, or a cached
   mode-20 font). Coordinates are signed pixels and
   everything clips to the 640x480 panel. Mode 36 composes inside a mode-8 batch
   like modes 15/19/20. See `patches/shapes.h` for the type table.
 * Mode 37 keeps a retained scene of up to 128 shape slots (slot order is paint
   order) that the glasses re-render themselves: `[37][flags][bg][ops...]` with
   ops to set, delete, show/hide or move a slot, GLIDE it by a delta over N
   frames, or TWEEN any parameters (including color and stroke width) to target
   values, each with a CSS-style cubic-bezier easing curve. Flag bit 0 renders
   and presents the scene; bit 1 clears it first. The scene renders into a
   CFW-owned full-panel frame, so animation frames never touch EvenHub container
   memory, and a lapsed framebuffer lease stops the animation timer. If heap 13
   cannot spare that 150 KiB frame, a commit still draws the scene into the
   container shadow and only animation is refused. The record grammar is
   documented at the top of `patches/scene.c`.
 * Mode 38 controls animation: freeze, set the frame period (10-250 ms, default
   33), release the scene, or finish every animation and present the end state.

All of these are implied by the firmware revision (see below); there are no
per-feature capability tokens.

Revision 21 adds retained compound SVG paths (mode-37 op 8) and duration-based
rotation of geometric shapes and paths (op 9). Paths support nonzero/even-odd
fills with holes, quadratic/cubic curves, and the existing move/scale/color
tweens. Rotation uses fractional unwrapped angles and an independent animation
channel, so shapes can move and rotate concurrently. The wire contract accepts
compiled paths, not XML; text/image rotation and path morphing are not included.
See [the protocol and test instructions](patches/VECTOR_PROTOCOL.md).

Revision 24 moves the local graphics packets to modes **36/37/38** and ANCS
fields to **125/126**, leaving upstream's sensor modes 16/17 and fields 105/106
unmodified. All record formats and operation numbers stay the same. Clients
must use the new numbers; old graphics packets are no longer accepted. See
[the wire mapping](patches/UPSTREAM_COMPATIBILITY.md).

Revision 25 drops the feature tokens: the field-100 string is just
`GLASSLYCFW/<n>`. Revision 26 makes upstream's fast BLE profile (7.5 ms
connection interval, slow mode disabled) a runtime choice through settings
field 127, defaulting to stock behaviour, and reports it in field 128.

Revision 31 removes the stock image-message hooks (snapshot FIFO, deferred
consumer, immediate ACK, 576x288 lift, per-message zlib and the BMP loader)
and the 16-bit texture modes 12/13/14: custom commands arrive only over the
SID `0xf0` transport, and shape records draw cached text with a mode-20 font.
Clients on the old path get no response (the stock firmware image handler
runs); there is no migration.

Revision 30 merges upstream jimrandomh/g2flash main (`Faceclaw/4` through
`Faceclaw/14`, ported from its 2.2.9.22 base to 2.2.10.10): the SID `0xf0`
transport, transport-level compression with range ACKs, modes 18/19/20 over a
256 KiB texture cache, compass calibration preservation, free/max heap
statistics on the debug overlay and an owned panel shadow. Unlike upstream,
the stock image path is not stripped; both paths stay live.

Revision 27 makes panel ownership explicit. Any raster mode (3/6/9/15/19/20/36)
stops and freezes a running scene animation, and when the last present came
from the scene's own frame it first copies that frame into the container shadow
so deltas compose onto what is on glass. A mode-37 patch may carry a TAG record
(op 10, `[10][0][tag:u16]`); the next COMMIT reports settings field 129 =
`[tagLo][tagHi]` once every animation it started has ended (immediately when
none did, or when a raster mode takes over). Modes 37/38 are accepted inside a
mode-8 bundle, where a COMMIT renders into the shadow the bundle presents.

Run the complete offline suite with
`python3 patches/host/run_vector_tests.py --out /tmp/g2-vector-tests`.
It tests the firmware C under sanitizers and replays the standalone shape and
Bad Apple SVG payloads before any mobile integration. After installing revision
24 on the glasses, `cd demos && bun vector-suite.ts --device` runs the visual
suite; add `--bad-apple` for the vector video demo.

Revision 20 adds an ANCS relay. On iOS the stock firmware already reads the
phone's notifications through the Apple Notification Center Service (the right
lens subscribes; the phone app itself never can) but only tells the app which
app posted one. The relay taps the stock ANCS client before its whitelist and
its 63-byte title copies and forwards every notification to the phone as
private sid-0x09 field-125 records: the Notification Source event (added,
modified, removed, with flags, category and UID), each attribute (app id,
title, subtitle, message, message size, date, action labels; chunked so no
message exceeds 150 bytes) and the app's display name. The phone enables it
with a fail-open 90-second lease on field 126 (`['A','N',1,op]`: enable,
disable, query, perform a positive/negative action on a UID, renew). Records
are queued on the BLE stack task and drained by a timer, so a slow link drops
whole records (counted in the STATUS reply) rather than stalling the stack. The
full contract is at the top of `patches/ancs_relay.c`.
`demos/shapes-demo.ts` exercises all three modes, and
`patches/host/shapes_host_test.c` renders every primitive on the host so the
rasterizer and easing math can be checked without glasses.

The firmware also adds a microphone control plane (capability tokens `micctl`,
`micmc`, `micraw`). Each temple carries a front + rear microphone pair, and the
stock firmware only ever sends the phone a mono 16 kHz LC3 stream. The mic
extension lets the phone choose, per temple, the capture front end (codec vs
PDM), which of the pair's microphones are used, the codec (LC3 vs raw PCM
passthrough), sample format/rate, and LC3 bitrate, over a private
settings-channel message (sid 0x09 field 103), with live read-back on field 104
so a UI (e.g. SybilSight's glasses -> microphones menu) can display and confirm
the active configuration of both temples. When armed, capture is streamed as
`'SM'` frames carrying the multi-channel samples, a millisecond timestamp, and
the on-device SSR + TDOA angle estimate — everything a phone-side beamformer
needs to do direction-of-arrival processing across the four microphones,
fused with the compass/IMU heading the firmware already forwards. On a
bilateral pair the right temple relays its two encoded channels to the left
over the wired inter-temple link (the audio manager's common-data channel), so
the phone receives all four microphones as one four-channel LC3 stream on a
single Bluetooth connection rather than pairing two independent per-temple
streams; each relayed chunk carries the inter-temple time offset the left
temple measured, and a relay-statistics record (`'RS'`) reports pairing and
loss so a dropped chunk shows up as a sequence gap on the phone instead of
vanishing silently. Because the stock BLE message queue drops a streaming
notification without warning once it is half full, the relay withholds and
counts a frame when the queue nears that watermark instead of feeding the
silent drop. Streaming is
held by a fail-open 90-second renewal lease, so mics can never be left running
when the phone goes away. Bringing up the capture hardware is additionally
gated behind an explicit arm flag because several of the recovered stock audio
entry points are ABI-inferred and must be validated on hardware first; see the
contract comment in `patches/mic_control.c`.

The firmware also enables LE 2M PHY support and offers a phone-selectable
fast link: settings field 127 (`['B','L',1,op]`, op 0 = stock, 1 = fast,
2 = query, sent to each lens) switches between the untouched stock connection
behaviour (the default: 15-30 ms fast profile, then the one-minute slow-mode
timer) and a 7.5 ms / latency-0 profile with slow mode suppressed, which keeps
screen transfers from being throttled at a battery cost. Every settings read
reports the state in field 128, including the interval the central actually
granted. Mode 11 cleanup returns the link to stock. It also asks the
controller for LE Data Length Extension (251-byte link-layer payloads) on the
phone link when a microphone session arms, so a four-channel audio
notification travels in a single radio packet instead of several — the stock
host never raises its own transmit length. Connection parameters still depend
on the phone; it must request 2M PHY, must agree to the short connection
interval, and (for the larger data length) must not refuse the length update.
See `patches/ble_link.c`.

Some other features this has (used by Faceclaw, but the exact API may not be
fully documented):

 * Receive ring and temple-touchpad long-press and long-press-release as regular
   source-qualified gestures, rather than opening a modal offering to quit
 * Receive 2.2.10's tap-then-long gesture as distinct private event type 11 and,
   while the framebuffer lease is held, suppress its incompatible stock Menu path
 * Play sound effects with the piezo buzzer
 * Receive on-head detection wear/unwear events, to trigger a lock-screen
 * Use the magnetometer as a compass. Since revision 30 (upstream `Faceclaw/14`)
   the current magnetic calibration accuracy is preserved across IMU
   reconfiguration while the Faceclaw framebuffer lease is valid, including
   compass stop/start cycles, instead of being reset to zero by the stock
   sensor-parameter setter.
 * Read the ambient light sensor (a TI OPT3001 on the master temple) and,
   optionally, run it in a "passive" mode where the firmware polls the sensor
   for the phone but the stock auto-brightness adjuster never steps the panel,
   so the phone can implement its own brightness policy (image-handler mode 16;
   readings arrive as settings-channel field 105). See the contract comment in
   `patches/als_sensor.c`.
 * Take over the wakeword ("hey Even") and replace what it opens with a
   different phone-side transcription and AI agent pipeline
 * Take over the screen-wake even on the dashboard, so that you can end the
   EvenHub session (putting the glasses in a low-power mode) and return to
   Faceclaw with a double-tap
 * While the glasses are in that low-power mode (no app on screen), also
   report the single tap, long press and long-press release that the stock
   display thread drops, so the phone can show a lightweight sleep-time
   display (Faceclaw's Glanceboard) without a full wake
 * Report the head-up (IMU head-tilt) wake distinctly from the double-tap
   wake, and forward the head-up while an EvenHub page is on screen, so the
   phone can route it to that lightweight display too

The SID `0xf0` transport described above needs `GLASSLYCFW/31` on both lenses;
`send_message_probe.py` documents the packet format, lens selection and ACK
verification in its docstring and `--help`.

Glasses with a custom firmware identify themselves with the version number of
the stock firmware that the modded version is based on, with an extra field in
the settings-response message (protobuf field 100) carrying a firmware revision
string of the form `GLASSLYCFW/<n>` (upstream Faceclaw builds use `Faceclaw/<n>`
with the same scheme). See `settings_send_wrapper` in `patches/settings_ext.c`.
`<n>` goes up each time the firmware contract changes and is not kept in sync
with Glassly phone-app versions; the phone app requires a specific revision and
offers to reflash when the installed one is older. Older builds advertised
`GLASSLYCFW/<n>` or `EVENCFW/<n>` followed by feature tokens instead. If writing
your own firmware and your own phone software to go with it, use a different
prefix, and assume that firmware is only compatible if you recognize the exact
string.

Custom firmwares in this repository are intended to be backwards-compatible
with the official Even Realities app; ie, if the phone doesn't send any
bluetooth messages that make use of the added features, it will behave the same
way. In practice, this isn't tested, and using a custom firmware with the
official Even app may introduce bugs. EvenHub apps are especially likely to be
broken by custom firmware, since most of the modifications are to
EvenHub-related functionality.

Custom firmwares are currently _not_ expected to be compatible with each other.
When you update Faceclaw, you should update the firmware at the same time. If
you are writing your own software that relies on custom firmware, you probably
want to specify a particular CFW version. (This will probably change in the
future, when there is less new stuff still to be added.)

What _is_ tested for compatibility is the OTA updater. This firmware does not
make any changes to how OTA updates are installed. If you connect the official
Even app to your glasses, and it has a firmware that's newer than the one your
CFW is based on, it will offer to install an update. Installing an OTA update
using the official Even app will fully remove the custom firmware and restore
it to stock behavior. You can also go back to stock firmware by installing an
unmodified firmware image using g2flash, or the "Uninstall firmware" menu item
in Faceclaw.


## Using the Flash Tool

`g2flash.py` flashes firmware onto Even Realities G2 smart glasses by
reimplementing the official app's BLE flash protocol. It is the tool used to
push custom firmware (a patched `*_cfw.bin` image) onto the device.

> **WARNING — this voids your warranty and can brick the glasses.**
> Flashing custom firmware over the OTA path carries a real risk of bricking
> the device. The tool makes you type `my warranty is void` at an interactive
> prompt before it will write anything (use `--my-warranty-is-void` to skip the
> prompt for automation). Only proceed if you understand and accept the risk.

## Quick start

```bash
cd g2flash
./build_cfw.sh                       # set up venv, download stock fw, patch, verify
./venv/bin/python g2flash.py -c g2://local -f g2_2.2.10.10_cfw.bin
```

`build_cfw.sh` does the whole build: it creates `./venv` with the flasher's
dependencies, downloads the stock **G2 2.2.10.10** firmware from Even's CDN,
applies the patches in `patches/`, and verifies that both the download and the
patched result match pinned SHA-256 hashes (so a clean run proves you got
exactly the reviewed image). Run `./build_cfw.sh --help` for options
(`--skip-venv`, `--force-download`). Then flash as shown above — see
[Usage](#usage) for connection strings and safety flags.

## What's in this directory

- `g2flash.py` — the flasher.
- `build_cfw.sh` — one-shot venv setup + download + patch + verify (see above).
- `patches/` — the patch sources and tools:
  - `cfw_patches.json` — the **committed patch set**: a list of
    offset/expected-old/new byte patches (plus base + output SHA-256s) that turns
    the stock image into the CFW image. This is the source of truth for the build.
  - `apply_patches.py` — replays `cfw_patches.json` onto the stock image. **No
    compiler** — pure Python stdlib, so it runs anywhere (a phone, a fresh box).
    This is what `build_cfw.sh` uses to produce the image.
  - `gen_patches.py` — compiles the injected code with **clang** and (re)generates
    `cfw_patches.json`. Run it after editing the patch sources:
    `python3 patches/gen_patches.py g2_2.2.10.10.bin patches/cfw_patches.json`
    (or `./build_cfw.sh --update-patches`), then commit the JSON.
  - `patch_compress.py` — the all-in-one patcher (576 carrier lift + image
    compression + direct framebuffer presentation + capability field);
    `gen_patches.py` calls it to build the ops.
    Holds every stock-firmware address the patches depend on; see
    `patches/REBASE-2.2.10.10.md` (this fork's 2.2.9.22 -> 2.2.10.10 rebase) and
    `../notes/fw-2.2.9.22-cfw-rebase.md` (the earlier 2.2.6.10 -> 2.2.9.22 rebase)
    for how they were derived.
  - `build.py`, `*.c` — the C→position-independent-Thumb pipeline and sources
    for the injected firmware code (compiled by `gen_patches.py`; the resulting
    machine code lands in `cfw_patches.json`).
- `requirements.txt` — the flasher's Python dependencies.

Firmware images (`g2_2.2.10.10*.bin`) are **not** checked in — they are Even's
firmware, so you build them locally with `build_cfw.sh`.

## Requirements

- Python 3.x
- One of two transports to reach the glasses:
  - **local** — this machine's own Bluetooth radio, via the `bleak` package.
  - **droidbridge** — a bonded Android phone running
    [DroidBridge](../droidbridge) that forwards GATT over HTTP/WebSocket; uses
    the `websocket-client` package.

Third-party Python dependencies:

| Package            | Needed for                          | Imported as |
|--------------------|-------------------------------------|-------------|
| `bleak`            | `g2://local` transport              | `bleak`     |
| `websocket-client` | `g2://droidbridge` transport        | `websocket` |

Both are imported lazily, so you only need to install the one for the transport
you actually use. Firmware parsing, validation, and `--recompute-checksums` run
on the standard library alone.

## Setting up the venv

`build_cfw.sh` creates and populates `./venv` for you as part of a normal run.
To set it up by hand instead:

```bash
cd g2flash
python3 -m venv venv
./venv/bin/python -m pip install --upgrade pip
./venv/bin/python -m pip install -r requirements.txt
```

With the venv activated (`source venv/bin/activate`) you can invoke the tool as
`python g2flash.py ...`; otherwise use `./venv/bin/python g2flash.py ...`. Run
`deactivate` to leave the environment.

### macOS note

On macOS, `bleak` talks to CoreBluetooth, which never exposes BLE MAC
addresses — scanned addresses are random per-host UUIDs. `g2flash` works around
this by scanning and matching the last three MAC bytes embedded in the arm's
advertised name (`Even G2_32_L_693CCB`). For a local flash the arm must be
powered on and **not** connected to the phone (quit the Even app / turn off the
phone's Bluetooth) so it advertises for a direct connection. The first time you
run it, macOS will prompt to grant your terminal Bluetooth permission.

## Usage

```
python g2flash.py -c <connection-string> -f <firmware.bin> [options]
```

Connection strings:

```
# direct from this machine's Bluetooth radio (needs bleak)
g2://local?left=<addr>&right=<addr>&addressType=public|random

# through a bonded phone running DroidBridge (needs websocket-client)
g2://droidbridge?phone=<host>&port=<port>&token=<tok>&left=<mac>&right=<mac>
```

`addressType=public` is a normal MAC (`D0:7A:47:82:09:67`); `random` is the
macOS/CoreBluetooth peripheral-UUID style.

Common options:

- `--lens left|right|both` — which arm to flash (default `both`).
- `--stop-before discover|heartbeat|file_check|flash|done` — dry-run gate that
  halts before the named stage; use it to test connectivity without writing.
- `--my-warranty-is-void` — skip the interactive warranty confirmation.
- `--component-retries N` / `--block-nak-retries N` — transfer retry tuning.
- `--reconnect-attempts N` / `--reconnect-delay SECONDS` — recovery tuning when
  an ACK timeout indicates that the arm dropped its GATT connection.
- `--debug` — print received BLE frames.

`--recompute-checksums IMAGE` rewrites an image's stored checksums in place
(component CRC32C + mainApp preamble CRC32) to match its current payloads and
exits without connecting. Run it after any length-preserving binary patch —
otherwise the glasses reject the component on END with status 7 (CHECK_FAIL).

### Examples

```bash
# dry run: connect to both arms over the local radio and stop before any write
python g2flash.py \
  -c 'g2://local?left=AA:BB:CC:11:22:33&right=AA:BB:CC:44:55:66&addressType=public' \
  -f g2_2.2.10.10.bin --stop-before flash

# fix checksums after patching, no device needed
python g2flash.py --recompute-checksums g2_2.2.10.10_cfw.bin

# flash the custom firmware to both arms via DroidBridge
python g2flash.py \
  -c 'g2://droidbridge?phone=192.168.1.50&port=8080&token=secret&left=AA:BB:CC:11:22:33&right=AA:BB:CC:44:55:66' \
  -f g2_2.2.10.10_cfw.bin
```

## How it works (brief)

The flasher speaks the same `aa21`-framed envelope protocol as the official
app, validated byte-for-byte against a real flash capture. The firmware image
is an EVENOTA container of five or six components; each is streamed over the firmware
data service (`...e1001`) as a FILE_CHECK subheader followed by 4 KB blocks,
then an END check the glasses verify against a per-component CRC32C. Before
BEGIN, it performs the stock control-channel authentication handshake; omitting
that handshake appears to cause firmware 2.2.10 to close an otherwise busy GATT
connection after about 30 seconds. OTA data fragments refresh the transfer watchdog. The
official OTA capture sends no EvenHub-control heartbeats between BEGIN and the
final END, so neither does this flasher. DroidBridge flashes also reuse one HTTP connection
instead of opening one per BLE fragment. If an ACK is lost, the flasher never
risks replaying that ambiguous block in place: it reconnects, restores
notifications, sends a fresh BEGIN, and restarts the entire component from
FILE_CHECK. Arms are flashed one at a time. See the module docstring and comments
in `g2flash.py` for the wire-level details and the retry/recovery rationale.


## Contributing

PRs welcome, especially of firmware mods that unlock functionality that isn't
usable in the stock firmware. Please only submit changes that you've tested on
a real device.


# Acknowledgements

Thanks to kalanihelekunihi for [evenRealities-openCFW](https://github.com/kalanihelekunihi/evenRealities-openCFW/) and Commute773 for [g2-kit-unofficial](https://github.com/Commute773/g2-kit-unofficial/), which were immensely helpful while creating this.

### R1 battery reporting

The stock R1 battery cache is available in settings field 106 through the exact
read-only image-handler query `[17,0]`. The firmware also sends this report
after a settings read, as a separate notification rather than inside the
settings reply. Detect support by revision 24 or later, or by the `RB` report.
See [the wire contract and stock-firmware evidence](docs/ring-battery.md).
