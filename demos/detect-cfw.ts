#!/usr/bin/env bun
// Detect whether the glasses are running our custom firmware, and which
// revision it advertises.
//
// The CFW appends protobuf field 100 ("GLASSLYCFW/<n>") to the sid=0x09
// settings READ response; stock firmware never sends it. `queryGlasslyCfw`
// does the read and parses that field. Absence => stock firmware.
//
//     bun detect-cfw.ts
//
// Expected on CFW:
//     firmware: L=2.2.9.22 R=2.2.9.22
//     CFW detected: GLASSLYCFW/26
//       revision 26 (required: 26)
// Expected on stock:
//     no CFW capability field — stock firmware (or pre-caps CFW build)

import { G2Session, querySettings } from "g2-kit/ble";
import { queryGlasslyCfw, REQUIRED_REVISION } from "./glassly-cfw";

const session = await G2Session.open();

const settings = await querySettings(session, 100);
if (settings) {
  console.log(`firmware: L=${settings.leftSoftwareVersion} R=${settings.rightSoftwareVersion}`);
}

const cfw = await queryGlasslyCfw(session, 101);
if (!cfw) {
  console.log("no CFW capability field — stock firmware (or pre-caps CFW build)");
} else {
  console.log(`CFW detected: ${cfw.raw}`);
  const state = cfw.revision >= REQUIRED_REVISION ? "" : " — older than the demos here expect";
  console.log(`  revision ${cfw.revision} (required: ${REQUIRED_REVISION})${state}`);
}

await session.close();
process.exit(0);
