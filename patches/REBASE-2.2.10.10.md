# CFW rebase: stock G2 2.2.9.22 → 2.2.10.10

This fork rebases the g2flash custom-firmware patch set from the upstream
**2.2.9.22** base onto stock **G2 2.2.10.10**, and folds in the SybilSight /
Faceclaw four-microphone-array and display-pipeline work. The build is the
plain g2flash process — `./build_cfw.sh` downloads and verifies the 2.2.10.10
stock image, `gen_patches.py` (clang) regenerates `cfw_patches.json` from the
sources, and `apply_patches.py` replays it with no compiler and verifies the
pinned output hash.

## What changed vs upstream 2.2.9.22

* **Base:** stock `g2_2.2.10.10.bin`
  (sha256 `927879057685a4147c6ba1fe33e5f3740d3cc48f87141a9039204d94516e65b8`,
  CDN md5 `5d2abaf086ad7cc4709cad679b7b24d1`). The main-app component start
  moved by 53 bytes vs 2.2.9.22 (smaller bootloader), so `DELTA` in
  `patch_compress.py` went `0x379BFE` → `0x379C33`.
* **Addresses:** every `FW_* ((type)0x…U)` entry point and RAM anchor in the
  `.c`/`.h` sources, and every site tuple/dict, the ring-battery ABI table and
  the 20-entry TLSF-tail false-positive map in `patch_compress.py`, was
  re-derived against the 2.2.10.10 disassembly — code sites by instruction-
  window match confirmed with a `bl`-target decode, RAM globals through the
  instruction that loads them. Each mapping is recorded, with the stock
  signature bytes at both the old and new address, in
  `rebase-2.2.10.10-address-profile.json` (the machine-checkable review
  artifact this rebase was validated against; its `candidateVersion` field
  reflects the SybilSight build lineage and is not used here).
* **Features folded in** (see the source files and the README Modifications
  section): the four-microphone relay (the right temple relays its two encoded
  channels to the left over the inter-temple common-data link, so the phone
  receives one four-channel LC3 stream on a single connection), an LE Data
  Length request on the phone link, a dirty-row partial panel refresh and a
  session-persistent inflate stream, relay-queue back-pressure, and the `'RS'`
  relay-statistics record. Touched sources: `mic_control.c`, `zlib_glue.c`,
  `cfw_context.[ch]`, `rle.c`, `debug.c` (plus address-only rebasing of the
  rest).

## Version identity

Per the g2flash convention (README "How custom firmware identifies itself"),
this CFW reports the **stock version number, 2.2.10.10**, plus the `Faceclaw/3`
capability marker in `settings_send_wrapper` — it does not rewrite the reported
version string. The functionally identical image that was exercised on hardware
through the SybilSight webflasher instead reported a distinct `2.2.10.72`
package identity; the two differ only in that version string and the three
whole-component CRC fields that cover it. If a distinct reported version is
wanted here, bump `Faceclaw/<n>` in `settings_ext.c` rather than rewriting the
stock version.

## Verification

* `gen_patches.py` (clang, thumbv7em) regenerates `cfw_patches.json`
  byte-for-byte — `build_cfw.sh` prints "clang reproduces the committed patch
  set exactly".
* `apply_patches.py` reproduces `g2_2.2.10.10_cfw.bin`, sha256
  `ce2be9349b7f1c17384bfcef4cfa241814fa20e1d1103bb0668f71fa08245f0f`
  (`OUT_SHA256` in `build_cfw.sh`).
* The 33 patch operations equal the hardware-tested SybilSight 2.2.10 recipe's
  functional operations exactly, except the three whole-component CRC fields
  (which cover the differing version string) — i.e. the firmware behavior is
  the image validated on hardware.

## Reproduce

```
./build_cfw.sh                 # download + verify stock, apply, verify output
./build_cfw.sh --update-patches  # regenerate cfw_patches.json from sources (needs clang)
```

## 2026-09-18: upstream merge (Faceclaw/4 → Faceclaw/14) on top of this base

Upstream `jimrandomh/g2flash` stayed on 2.2.9.22 and replaced its stock
image-message hooks with a private SID-0xf0 transport. This fork keeps the
stock path and adds the transport as a second ingress (revision
`GLASSLYCFW/30`). The new sites were ported with the same masked
instruction-window method; every window in `validate_message_transport_stock`
and `validate_compass_calibration_stock` was re-read from the 2.2.10.10 image
after the match, and both compass digest regions are byte-identical.

| Site / callee | 2.2.9.22 | 2.2.10.10 | Evidence |
| --- | --- | --- | --- |
| `MESSAGE_RX_BL_SITE` (bl TPL_ReceivePacket) | `0x4d335a` | `0x4d5a16` | 18-byte window at `0x4d3350` unique at `0x4d5a0c`; bl decodes to `0x4ce54c` |
| `CFW_STOCK_RECEIVE` | `0x4cf3e8` | `0x4ce54c` | prologue unique |
| `MESSAGE_BRIDGE_BL_SITES` | `0x45e16e`, `0x45e2ca`, `0x4602c8` | `0x45e4ee`, `0x45e64a`, `0x460648` | all decode to `0x45d520`; +0x380 region delta |
| `CFW_STOCK_BRIDGE_RECEIVE` | `0x45d1a0` | `0x45d520` | prologue unique |
| `CFW_BRIDGE_SEND` | `0x46a58c` | `0x46ab70` | prologue unique (+0x5e4, as `0x46a73c` → `0x46ad20`) |
| `CFW_BLE_SEND` | `0x47d72c` | `0x47e9c8` | +0x129c, the delta of `ALS_FW_SEND`/`COMPASS_SEND` |
| `CFW_LENS_SIDE` | `0x45cfdc` | `0x45d35c` | already mapped |
| osMutex new/take/give/delete | `0x442ef6`… | unchanged | byte-identical |
| `COMPASS_ACCURACY_RESET_SITE` | `0x4b4c76` | `0x4b6b5a` | +0x1ee4 with its prologue `0x4b4450` → `0x4b6334`; literal pool `0x4b55cc` → `0x4b74b0` = {`0x20077400`, `0x20076220`} |
| vendor mag-bias setter (digest, 190 B) | `0x51bdc6` | `0x51e66a` | same sha256 |
| IMU FIFO bias/accuracy block (digest, 116 B) | `0x4b6936` | `0x4b881a` | same sha256 |
| zlib init/inflate/end, "1.1.4" | `0x5d6167`/`0x5d6235`/`0x5d612b`/`0x7b75f8` | `0x5d8e57`/`0x5d8f25`/`0x5d8e1b`/`0x7bb6f8` | existing mapping |

### Revision 31

`LOADBMP_BL_SITE` (0x4a5736), `SNAPSHOT_BL_SITE` (0x4ee982),
`IMAGE_ACK_SUCCESS_SITE` (0x4ee9a2) and the three container size-lift edits
(0x4f0666/0x4f072e/0x4f0732) are retired: those stock bytes are left as-is and
custom commands enter only through the SID-0xf0 sites above.
