# Rebase 2.2.10.10 -> 2.3.0.24 (GLASSLYCFW/37, 2026-09-19)

Upstream jimrandomh/g2flash moved to stock G2 2.3.0.24 in `c1a4b55` and replaced its
hardcoded address tables with `stock_abi_230.json` + `audit_stock.py` (an exact inventory
of every stock literal the sources use, byte witnesses for every code dependency, and the
ordered live-patch footprint; the build fails closed on any drift). This fork merged that
work and re-derived every fork-specific address the same way upstream did.

## Method

* Shared dependencies (everything upstream's own sources reference) came from upstream's
  table, chained through `rebase-2.2.10.10-address-profile.json` (2.2.9.22 -> 2.2.10.10)
  where our literal differed from upstream's old one.
* Fork-only code addresses were located by a masked Thumb-2 instruction-window match
  (BL/B/MOVW/MOVT/LDR-literal/ADR immediates masked, everything else exact) from the
  2.2.10.10 image into 2.3.0.24, then confirmed by decoding the callee of every hooked
  `bl`. Everywhere both methods applied they agreed (0 disagreements over 118 literals).
* Fork-only RAM globals were re-derived through the pc-relative loads that materialise
  them (match the loading instruction window, read the new literal); the SRAM deltas are
  not uniform in 2.3.0.24 (+0x78a..+0x78d for the BLE mode bytes, +0x1038..+0x104c for
  the connection records, +0x106b/+0x106c for the BLE flag block), so nothing was shifted
  by a constant.
* Each new dependency has a `stock_guard_size` 16 witness in `stock_abi_230.json`
  (`method` "instruction window" or "literal loader witnesses", `review` marks it as a
  fork dependency). `validate_ble_link_stock` pins 26 windows of the connection-parameter
  machinery; the ring-battery, transport and compass validators are upstream's.

## What changed in 2.3.0.24 that mattered

* **BLE connection requests carry a 16-bit generation** (`0x2007835c`). The wanted-mode
  setter (`0x47b52f`) bumps it, `_connectParamReq_impl(mode, generation)` drops stale
  requests, and the deferred callback (`0x47c451`) takes `mode | generation << 8`.
  `ble_link.c` packs the generation into its posted request exactly like stock; hook 1
  now tail-calls the connection getter at `0x475400`. The already-fast classifier and the
  connection record offsets (+0x18 interval, +0x1a latency) are unchanged.
* **The BLE mode byte block was reordered** (`0x2000550b` last, `..0c` accepted,
  `..0d` wanted, `..0e` new, `..0f` applied); the BLE flag block gained a byte
  (`0x2007840f`), so fast-budget/fast-mode/pending moved to `0x20078410/11/13`.
* **Idle input forwarding**: 2.3.0 dropped the separate idle-mode call, so
  `faceclaw_idle_input_forward` runs from `headup_gate_impl` when the stock idle gate
  returns 1 (upstream's change; the 2.2.10.10 `faceclaw_idle_input_gate` shim is gone).
* **Gesture dispatcher**: upstream re-hooked the three menu-state getters instead of the
  UI poster and added ring touch-down forwarding (SysEvent 14; 13 is burned).
* **ANCS**: upstream added its own relay (raw GATT pipe on the EUS characteristic that
  takes the ANCS client away from stock while enabled). It carries none of the attribute,
  app-name, status, query or renew records the Glassly app consumes, so this fork keeps
  its `ancs_relay.c` (settings fields 125/126) and its five ANCC profile `bl` hooks,
  relocated (`0x4d7ab8`, `0x4d7ab0`, `0x4d8a6c`, `0x4d89e4`, `0x4d8b96`; anccCb
  `0x20065cf0`). Upstream's `cfw_ancs_*` sites are not applied.
* **Fast link**: upstream forces the fast profile unconditionally (rodata + impl
  patches). This fork keeps its phone-selectable toggle (field 127) and does not apply
  those two patches.
* **Mic relay (PR #4's four-microphone streaming)**: all sixteen fork-only mic literals
  re-derived; the common-data table entry `{0x010C, AUDM handler, 0}` moved to
  `0x6c626c` (handler `0x0056e6eb`). The codec request poster was restructured by stock
  (`0x47d6cd` now posts to the BLE task) but keeps the `uint32 auto_slow` ABI.
* The appended blob ends at `0x007e5f74`, 40 KiB under the MRAM ceiling.

## Build

```
./build_cfw.sh --update-patches      # stock 2.3.0.24 from Even's CDN, audit, clang, apply
SDKROOT=... python3 patches/host/run_vector_tests.py --out /tmp/host
```

Output `g2_2.3.0.24_cfw.bin`, sha256 pinned in `build_cfw.sh`.
