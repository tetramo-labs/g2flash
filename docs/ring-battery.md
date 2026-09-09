# R1 battery wire contract

Send the image-handler packet `[17,0]` (exactly two bytes). The master lens sends
a sid-9 settings notification with command ID 3, magic 0, and bytes field 106:

`['R','B',1,flags,level]`

Flags are connected=1, valid=2, charging=4. Level is 0–100 when valid and 255
when disconnected or the stock cache is invalid. The slave lens sends nothing.
The implementation reads the existing cache without opening a ring connection
or changing its reporting cadence.

In this combined revision-24 build the same notification follows a successful
settings read. It is separate from the settings reply to stay within the known
working BLE packet size. Feature discovery uses revision 24 or the `RB` report;
the capability string keeps the existing graphics tokens instead of adding
upstream's `ringbat17` token. Local scene packets use mode 37 and cannot collide with upstream mode 17.

## Stock ABI evidence (G2 2.2.9.22)

Upstream's `ring_battery.c` identifies the cache setter at `0x512d84`, accessors
at `0x512da2`, and the dashboard getter at `0x4a9be2`. They use RAM `0x200772a6`
for the battery level and its next byte for charging. The dashboard connection
predicate is `0x47efa8`, using connection-bit getters at `0x47eeb2`.

`validate_ring_battery_stock()` in `patches/patch_compress.py` checks the exact
stock instruction bytes and address literals at these sites before generating
any patches. The host regression suite covers every cached byte value in both
connection and charging states, invalid requests, and master/slave routing.
