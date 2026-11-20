#!/usr/bin/env python3
"""Drive the G2 CFW object cache through its compiled code in Blackwall's Renode model.

The CFW main image boots from its reset vector on Blackwall's Faceclaw 38 board
model (the same stock 2.3.0.24 base) until the stock UI draws its first frame.
The runner then borrows the UI task at the entry of its periodic app callback
and calls cfw_receive_packet, the function the CFW installs in place of the
stock TPL_ReceivePacket call, with phone-shaped packets. Settings-channel
packets (sid 0x09, the FB_ACQUIRE/FB_RELEASE lease) enter the same function and
continue on the stock path to settings_decode_wrapper. Replies are read at the
stock BLE notification sender, frames at the physical 640x480 framebuffer.

The board model covers about 1.65 s of virtual time after the first stock frame:
then the display task writes MSPI0 (0x40060000), which the model refuses, and
the machine stops. Scenarios share that budget (the runner prints what it used),
and the presenting SHOW runs last. Lease expiry is forced by moving the lease
deadline to the current OS tick; the expiry handling that follows is unmodified.
The emulated board is the right lens, so packets carry the right-lens bit.

Prerequisites: Docker, the pinned Blackwall Renode image, a Blackwall checkout
(--blackwall), clang able to build patches/patches_main.c, and the CFW OTA image.

    docker pull --platform linux/arm64 ghcr.io/hmmhmmhm/blackwall-renode@sha256:4f5f20c390e8c0d2b6336cdb708023cd2110e98ebaed9ae59f995bc057cbc0f8
    python3 patches/host/blackwall/run_cache_scenarios.py

After a firmware change, rebuild the image first (./build_cfw.sh --skip-venv, or
patches/gen_patches.py then patches/apply_patches.py); the runner refuses an image
whose injected blob differs from what patches/build.py compiles now. Each run
writes firmware copies into a new directory under --runs, which must stay
outside every git repository. Exit status is 0 only if every scenario passes.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / 'patches'))
from g2flash import crc16, parse_firmware_segments  # noqa: E402
from patch_compress import APP_LOAD_ADDR, APP_PREAMBLE  # noqa: E402

DOCKER_IMAGE = ('ghcr.io/hmmhmmhm/blackwall-renode@sha256:'
                '4f5f20c390e8c0d2b6336cdb708023cd2110e98ebaed9ae59f995bc057cbc0f8')
MODEL_FILES = {
    'activation.cs': '3dea34f8c0be5bcc9e3b5ea548a1be5a80d74dc6942f92bad8c06c9f3e6140f5',
    'board.repl': 'a49cff74fa410454ce274bd8d05f5b392f26128a1541fce501fe35015db7207c',
    'run.py': 'd852a1390bf6d2e2f1b967ae88654a6d540a70adf9b02053f5529ea41e595908',
    'strict.cs': '45f01e572d6076b1d14b982e44937e7bde45b35fc981de4f7208f23dc7b67598',
    'touch-profile.json': '5b90c0397ebdaf8dbbae1730260c645a00065c7cd690bcd3a28f6a623ae64fcf',
}
FACECLAW_MAIN_LENGTH = 3829832
MAIN_VECTORS = (0x2007fb00, 0x601a3f)

STOCK = {
    'ble_send': 0x0047ee0a,
    'bridge_send': 0x0046a860,
    'ui_callback': 0x004ad7e4,
    'tcb_slot': 0x20077e38,
    'os_tick': 0x20077e4c,
    'framebuffer_slot': 0x200008b8,
    'ctx_slot': 0x2029f59c,
}
CTX_MAGIC = 0xC0FFEE72
CTX_FIELDS = ('direct_lease_deadline', 'cache_epoch', 'scene', 'texture_cache', 'cache_lost', 'direct_active',
              'gate_timeouts')
PANEL_W, PANEL_H = 640, 480
RIGHT_LENS = 2
FIRST_FRAME_BUDGET_MS = 1650
SCRATCH = 0x21000000

TAG_RECEIVE, TAG_SEND, TAG_BRIDGE, TAG_SETTINGS, TAG_LOSE, TAG_REAP, TAG_DISPATCH, TAG_PRESENT = 1, 2, 3, 4, 5, 6, 7, 8
WATCHES = [
    (TAG_RECEIVE, 'cfw_receive_packet', 1, 2, 300),
    (TAG_SEND, 'ble_send', 2, 3, 300),
    (TAG_BRIDGE, 'bridge_send', 1, 2, 300),
    (TAG_SETTINGS, 'settings_decode_wrapper', -1, -1, 0),
    (TAG_LOSE, 'cfw_cache_lose', -1, -1, 0),
    (TAG_REAP, 'cfw_cache_reap', -1, -1, 0),
    (TAG_DISPATCH, 'cfw_scene_dispatch', -1, -1, 0),
    (TAG_PRESENT, 'cfw_scene_present', -1, -1, 0),
]
STATUS = {0: 'APPLIED', 1: 'STALE', 2: 'MISSING', 3: 'CAPACITY', 4: 'REFUSED', 5: 'UNCHANGED',
          6: 'DELTA', 7: 'SNAPSHOT', 8: 'RESET'}

EPOCH, EXPIRY_EPOCH, PRESENT_EPOCH = 0x1234, 0x2222, 0x3333
ASSET_ID, OBJECT_ID = 0x00000101, 0x00000201
CHUNKED_ASSET_ID = 0x00000102
TEXT = 'CACHE 53'
TEXT_X, TEXT_Y = 220, 220


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main_body(image):
    segment = next(s for s in parse_firmware_segments(image) if s['fn'].endswith('s200_firmware_ota.bin'))
    payload = image[segment['off'] + 128:segment['off'] + 128 + segment['ps']]
    if struct.unpack_from('<I', payload, 0x14)[0] != APP_LOAD_ADDR:
        raise SystemExit('main-app preamble load address is not 0x438000')
    body = payload[APP_PREAMBLE:]
    if struct.unpack_from('<II', body) != MAIN_VECTORS:
        raise SystemExit('main vectors differ from the 2.3.0.24 base the board model was built for')
    return body


def cfw_symbols(body):
    built = subprocess.run([sys.executable, str(REPO / 'patches/build.py'), str(REPO / 'patches/patches_main.c'), '--json'],
                           capture_output=True, text=True, check=True, cwd=REPO)
    build = json.loads(built.stdout)
    text = bytes.fromhex(build['text'])
    for offset in range(len(body) - len(text), len(body) - len(text) - 8, -1):
        if body[offset:offset + len(text)] == text:
            base = APP_LOAD_ADDR + offset
            return {f['name']: base + f['offset'] for f in build['functions']}, base
    raise SystemExit('The image does not contain the blob patches/build.py compiles now. '
                     'Rebuild it with ./build_cfw.sh --skip-venv (or gen_patches.py + apply_patches.py).')


def context_layout():
    source = '#include <stddef.h>\n#include "cfw_context.h"\nconst unsigned layout[] = {\n'
    source += ''.join(f'  offsetof(customCfwContext, {name}),\n' for name in CTX_FIELDS) + '};\n'
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch) / 'layout.c'
        path.write_text(source)
        asm = subprocess.run(['clang', '--target=thumbv7em-none-eabi', '-mthumb', '-ffreestanding', '-O1', '-S',
                              '-I', str(REPO / 'patches'), '-o', '-', str(path)], capture_output=True, text=True, check=True)
    values = [int(v) for v in re.findall(r'^\s*\.long\s+(\d+)', asm.stdout.split('layout:', 1)[1], re.M)][:len(CTX_FIELDS)]
    return dict(zip(CTX_FIELDS, values))


def u16(v):
    return struct.pack('<H', v)


def u32(v):
    return struct.pack('<I', v)


def put_text(epoch, request):
    data = TEXT.encode()
    asset = bytes((0,)) + u32(ASSET_ID) + u32(1) + bytes((2,)) + u32(len(data)) + u32(0) + u16(len(data)) + data
    record = bytes((15, 1, 0x0f, 0)) + struct.pack('<hh', TEXT_X, TEXT_Y) + u32(ASSET_ID)
    return bytes((37,)) + u16(epoch) + u16(request) + asset + bytes((1,)) + u32(OBJECT_ID) + u32(1) + record


def show(epoch, request, flags):
    return bytes((38,)) + u16(epoch) + u16(request) + bytes((flags, 0, 0)) + u16(1) + u32(OBJECT_ID) + u32(1)


def show_nothing(epoch, request):
    """A presenting SHOW of no objects: applied, it blanks the panel."""
    return bytes((38,)) + u16(epoch) + u16(request) + bytes((1, 0, 0)) + u16(0)


def put_chunk(epoch, request, offset, data):
    """One chunk of an 8-byte string asset sent in two halves."""
    return (bytes((37,)) + u16(epoch) + u16(request) + bytes((0,)) + u32(CHUNKED_ASSET_ID) + u32(1) + bytes((2,))
            + u32(8) + u32(offset) + u16(len(data)) + data)


def cfw_revision():
    source = (HERE.parents[1] / 'settings_ext.c').read_text()
    return int(re.search(r'caps\[\] = "GLASSLYCFW/(\d+)"', source).group(1))


def control_reset(epoch, request):
    return bytes((41,)) + u16(epoch) + u16(request) + bytes((0, 244))


def state(epoch):
    return bytes((40,)) + u16(epoch) + u32(0) + u16(0)


class Stream:
    """Phone packets for one lens. Like send_message_probe.make_packets, each private
    message is its own stream (RESET first, END last), so its ordinal is 0."""

    def __init__(self, lenses):
        self.lenses = lenses
        self.sequence = 0x40
        self.settings_sequence = 0x10

    def packet(self, chunk, first, last):
        flags = self.lenses | (0x80 if first else 0) | (0x40 if last else 0)
        body = bytes((flags,)) + chunk
        packet = bytes((0xaa, 0x21, self.sequence & 0xff, len(body) + 2, 1, 1, 0xf0, 0)) + body + crc16(body)
        self.sequence += 1
        return packet.hex()

    def message(self, message):
        record = bytes((self.lenses | 8,)) + u16(len(message)) + crc16(message) + message
        offsets = range(0, len(record), 233)
        return [self.packet(record[o:o + 233], o == 0, o + 233 >= len(record)) for o in offsets]

    def settings(self, op):
        self.settings_sequence += 1
        control = bytes((0x46, 0x43, 0x01, op, 0, 0))
        package = bytes((0x08, 0x01, 0x10, 0x00, 0xaa, 0x06, len(control))) + control
        body = package + crc16(package)
        return [(bytes((0xaa, 0x21, self.settings_sequence, len(body), 1, 1, 0x09, 0x20)) + body).hex()]


def build_plan(symbols, lenses, layout):
    stream = Stream(lenses)
    steps = [dict(name='idle', op='run', ms=20, framebuffer=True)]

    def cfw(name, message, settle_ms=5, **extra):
        steps.append(dict(name=name, op='inject', packets=stream.message(message), settle_ms=settle_ms, **extra))

    def settings(name, op):
        steps.append(dict(name=name, op='inject', packets=stream.settings(op), settle_ms=40, ack_peer=True))

    settings('fb-acquire', 5)
    cfw('reset', control_reset(EPOCH, 1))
    cfw('put', put_text(EPOCH, 2))
    cfw('show', show(EPOCH, 3, 0))
    cfw('stale', show(EPOCH - 1, 4, 0))
    settings('fb-release', 6)
    cfw('released-state', state(EPOCH))
    cfw('released-show', show(EPOCH, 5, 0))
    settings('fb-reacquire', 5)
    cfw('after-release-state', state(0))
    cfw('after-release-show', show(EPOCH, 6, 0))
    cfw('expiry-reset', control_reset(EXPIRY_EPOCH, 7))
    cfw('expiry-put', put_text(EXPIRY_EPOCH, 8))
    steps.append(dict(name='expiry-force', op='expire'))
    cfw('expiry-query', state(EXPIRY_EPOCH))
    cfw('expiry-show', show(EXPIRY_EPOCH, 9, 0))
    settings('expiry-reacquire', 5)
    cfw('expiry-state', state(0))
    cfw('present-reset', control_reset(PRESENT_EPOCH, 10))
    cfw('present-put', put_text(PRESENT_EPOCH, 11))
    cfw('chunk-first', put_chunk(PRESENT_EPOCH, 20, 0, b'HALF'))
    cfw('chunk-last', put_chunk(PRESENT_EPOCH, 21, 4, b'WAYS'))
    cfw('chunk-resent', put_chunk(PRESENT_EPOCH, 22, 4, b'WAYS'))
    cfw('present-show', show(PRESENT_EPOCH, 12, 1), settle_ms=150, framebuffer=True)
    cfw('repeat-id-show', show_nothing(PRESENT_EPOCH, 12), settle_ms=150, framebuffer=True)
    cfw('fresh-id-show', show_nothing(PRESENT_EPOCH, 13), settle_ms=150, framebuffer=True)
    plan = dict(
        steps=steps, entry=symbols['cfw_receive_packet'], scratch=SCRATCH, borrow_pc=STOCK['ui_callback'],
        tcb_slot=STOCK['tcb_slot'], tcb_name_offset=0x34, os_tick=STOCK['os_tick'],
        framebuffer_slot=STOCK['framebuffer_slot'], framebuffer_bytes=PANEL_W * PANEL_H // 2,
        ctx_slot=STOCK['ctx_slot'], ctx_magic=CTX_MAGIC, ctx_layout=layout, borrow_wait_ms=400, call_wait_ms=1500,
        watches=[dict(tag=tag, pc=(symbols.get(name) or STOCK[name]) & ~1, data_register=d,
                      length_register=n, max_length=cap) for tag, name, d, n, cap in WATCHES])
    return plan


def stage(output, blackwall, body, plan):
    model = Path(blackwall) / 'models/faceclaw38'
    output.mkdir(mode=0o700, parents=True)
    (output / 'exports').mkdir(mode=0o700)
    for name, expected in MODEL_FILES.items():
        data = (model / name).read_bytes()
        if sha256(data) != expected:
            raise SystemExit(f'Blackwall model file changed since review: {name}')
        if name == 'run.py':
            needle = f',{FACECLAW_MAIN_LENGTH})'
            if data.decode().count(needle) != 1:
                raise SystemExit('run.py main-length argument not found')
            data = data.decode().replace(needle, f',{len(body)})').encode()
        (output / name).write_bytes(data)
    for name in ('cache_probe.cs', 'cache_driver.py'):
        (output / name).write_bytes((HERE / name).read_bytes())
    (output / 'probe.resc').write_text(
        'include @/work/strict.cs\ninclude @/work/activation.cs\ninclude @/work/cache_probe.cs\n'
        'mach create "glassly-cache-probe"\nmachine LoadPlatformDescription @/work/board.repl\n'
        'sysbus LoadBinary @/work/itcm-fill.bin 0\nsysbus LoadBinary @/work/sram-fill.bin 0x20000000\n'
        'sysbus LoadBinary @/work/main.bin 0x438000\ncpu VectorTableOffset 0x438000\n'
        'include @/work/run.py\ninclude @/work/cache_driver.py\nquit\n')
    (output / 'main.bin').write_bytes(body)
    (output / 'sram-fill.bin').write_bytes(b'\xa5' * 3670016)
    (output / 'itcm-fill.bin').write_bytes(b'\xa5' * 262144)
    (output / 'cache-plan.json').write_text(json.dumps(plan, indent=1))


def run_renode(output, timeout):
    inspect = subprocess.run(['docker', 'image', 'inspect', DOCKER_IMAGE, '--format', '{{.Architecture}} {{.Os}}'],
                             capture_output=True, text=True)
    if inspect.returncode or inspect.stdout.split() != ['arm64', 'linux']:
        raise SystemExit(f'Pinned Renode image missing. Run: docker pull --platform linux/arm64 {DOCKER_IMAGE}')
    command = ['docker', 'run', '--rm', '-i', '--network', 'none', '--memory', '1g', '--cpus', '1',
               '--pids-limit', '128', '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges',
               '--mount', f'type=bind,source={output},target=/work,readonly',
               '--mount', f'type=bind,source={output / "exports"},target=/work-output',
               DOCKER_IMAGE, '--disable-gui', '--console', '-p', '-e', 'quit', '/work/probe.resc']
    read_end, write_end = os.pipe()
    try:
        with (output / 'renode.log').open('w') as log:
            result = subprocess.run(command, stdin=read_end, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    finally:
        os.close(read_end)
        os.close(write_end)
    return result.returncode


def parse_send(row):
    data = bytes.fromhex(row['data'])
    sid = row['r'][1] & 0xff
    if sid != 0xf0 or not data:
        return dict(sid=sid, raw=row['data'])
    if data[0] == 5 and len(data) >= 15:
        return dict(sid=sid, kind='reply', stream=data[1], ordinal=data[2] | data[3] << 8, lens=data[4], mode=data[5],
                    request=data[6] | data[7] << 8, status=STATUS.get(data[8], data[8]),
                    epoch=data[9] | data[10] << 8, revision=struct.unpack_from('<I', data, 11)[0], extra=data[15:].hex())
    if data[0] in (1, 3) and len(data) >= 9:
        return dict(sid=sid, kind='ack' if data[0] == 1 else 'nack', stream=data[1], ordinal=data[2] | data[3] << 8,
                    lens=data[4], size=data[5] | data[6] << 8)
    return dict(sid=sid, raw=row['data'])


def framebuffer_png(fb, path):
    rows = []
    for y in range(PANEL_H):
        line = fb[y * PANEL_W // 2:(y + 1) * PANEL_W // 2]
        rows.append(b'\x00' + bytes(v for byte in line for v in ((byte >> 4) * 17, (byte & 15) * 17)))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', PANEL_W, PANEL_H, 8, 0, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


def lit_box(fb):
    xs, ys = [], []
    for i, byte in enumerate(fb):
        if byte:
            y, x = divmod(i, PANEL_W // 2)
            ys.append(y)
            xs += ([2 * x] if byte >> 4 else []) + ([2 * x + 1] if byte & 15 else [])
    return (min(xs), min(ys), max(xs), max(ys)) if xs else None


class Evidence:
    def __init__(self, report, output):
        self.steps = {step['name']: step for step in report['results']}
        self.output = output

    def ran(self, *names):
        return all(name in self.steps and not any('error' in p for p in self.steps[name].get('packets', []))
                   for name in names)

    def sends(self, name):
        return [parse_send(row) for row in self.steps[name]['rows'] if row['tag'] == TAG_SEND]

    def answer(self, name):
        """The cache reply and transport ACK/NACK sent while this step's message was processed."""
        mine = [s for s in self.sends(name) if s.get('ordinal') == 0]
        replies = [s for s in mine if s.get('kind') == 'reply']
        acks = [s['kind'] for s in mine if s.get('kind') in ('ack', 'nack')]
        return (replies[0] if len(replies) == 1 else None), acks

    def hits(self, name, tag):
        return [row for row in self.steps[name]['rows'] if row['tag'] == tag]

    def framebuffer(self, name):
        info = self.steps.get(name, {}).get('framebuffer', {})
        return (self.output / 'exports' / info['file']).read_bytes() if 'file' in info else None


def describe(answer):
    reply, acks = answer
    text = (f"{reply['status']} mode {reply['mode']} req {reply['request']} epoch 0x{reply['epoch']:04x} "
            f"rev {reply['revision']} lens {reply['lens']} extra {reply['extra'] or '-'}") if reply else 'no reply'
    return f"{text}; transport {'/'.join(acks) or 'silent'}"


def is_reply(answer, mode, status, request, epoch=None):
    reply, acks = answer
    return bool(reply and reply['mode'] == mode and reply['status'] == status and reply['request'] == request
                and (epoch is None or reply['epoch'] == epoch) and acks == ['ack'])


def epoch_of(answer):
    return answer[0]['epoch'] if answer[0] else None


def scenario_checks(ev):
    """Yield (scenario, steps it needs, check) where check returns (passed, detail)."""
    def lease():
        hits = ev.hits('fb-acquire', TAG_SETTINGS)
        ctx = ev.steps['fb-acquire']['context']
        ok = len(hits) == 1 and ctx.get('lease_deadline', 0) > ev.steps['fb-acquire']['start_os_tick']
        return ok, (f"settings_decode_wrapper calls {len(hits)}; lease deadline {ctx.get('lease_deadline')} "
                    f"at os tick {ev.steps['fb-acquire']['end_os_tick']}")
    yield 'FB_ACQUIRE over sid 0x09 arms the framebuffer lease', ('fb-acquire',), lease

    def reset():
        answer = ev.answer('reset')
        return is_reply(answer, 41, 'APPLIED', 1, EPOCH), describe(answer)
    yield 'RESET adopts the phone epoch', ('reset',), reset

    def put():
        answer = ev.answer('put')
        return is_reply(answer, 37, 'APPLIED', 2, EPOCH) and answer[0]['revision'] > 0, describe(answer)
    yield 'PUT string asset + TEXT object', ('put',), put

    def show_refs():
        answer = ev.answer('show')
        return is_reply(answer, 38, 'APPLIED', 3, EPOCH), describe(answer)
    yield 'SHOW resolves the cached (id, version) reference', ('show',), show_refs

    def stale():
        answer, shown = ev.answer('stale'), ev.answer('show')
        same_revision = bool(answer[0] and shown[0] and answer[0]['revision'] == shown[0]['revision'])
        epochs = (ev.steps['show']['context'].get('epoch'), ev.steps['stale']['context'].get('epoch'))
        presents = len(ev.hits('stale', TAG_PRESENT))
        ok = is_reply(answer, 38, 'STALE', 4, EPOCH) and same_revision and epochs == (EPOCH, EPOCH) and presents == 0
        return ok, describe(answer) + f'; revision unchanged {same_revision}, ctx epoch {epochs}, presents {presents}'
    yield 'Old epoch is answered STALE and changes nothing', ('show', 'stale'), stale

    def release():
        lost = ev.hits('fb-release', TAG_LOSE)
        ctx = ev.steps['fb-release']['context']
        query, gated = ev.answer('released-state'), ev.answer('released-show')
        moved = epoch_of(query)
        ok = (ctx.get('lease_deadline') == 0 and ctx.get('epoch') not in (None, EPOCH) and moved == ctx.get('epoch')
              and is_reply(query, 40, 'STALE', 0, moved) and gated == (None, ['nack']))
        return ok, (f"cfw_cache_lose calls {len(lost)}, lease deadline {ctx.get('lease_deadline')}; "
                    f"old-epoch STATE {describe(query)}; SHOW without lease {describe(gated)}")
    yield 'FB_RELEASE drops the lease, moves the epoch, NACKs lease-gated requests', (
        'fb-release', 'released-state', 'released-show'), release

    def moved():
        query, old = ev.answer('after-release-state'), ev.answer('after-release-show')
        epoch = epoch_of(query)
        ok = (query[0] is not None and query[1] == ['ack'] and epoch not in (None, 0, EPOCH)
              and is_reply(old, 38, 'STALE', 6, epoch))
        return ok, f'STATE {describe(query)} | old-epoch SHOW {describe(old)}'
    yield 'After re-acquire the moved epoch holds; an old-epoch SHOW is STALE', (
        'fb-reacquire', 'after-release-state', 'after-release-show'), moved

    def expiry():
        setup = ev.answer('expiry-put')
        forced = ev.steps['expiry-force'].get('expire', {})
        gated, query = ev.answer('expiry-show'), ev.answer('expiry-state')
        ctx = ev.steps['expiry-show']['context']
        epoch = epoch_of(query)
        ok = (is_reply(setup, 37, 'APPLIED', 8, EXPIRY_EPOCH) and 'error' not in forced and gated == (None, ['nack'])
              and ctx.get('lease_deadline') == 0 and ctx.get('epoch') not in (None, EXPIRY_EPOCH)
              and epoch not in (None, 0, EXPIRY_EPOCH))
        return ok, (f"setup PUT {describe(setup)}; deadline {forced.get('deadline_before')} forced to {forced.get('deadline_after')}; "
                    f"SHOW {describe(gated)}, then ctx epoch 0x{ctx.get('epoch', 0):04x} lease {ctx.get('lease_deadline')}; "
                    f"STATE after re-acquire {describe(query)}")
    yield 'Lease expiry (deadline forced to now) moves the epoch', (
        'expiry-reset', 'expiry-put', 'expiry-force', 'expiry-query', 'expiry-show', 'expiry-reacquire', 'expiry-state'), expiry

    def expired_query():
        query = ev.answer('expiry-query')
        before = ev.steps['expiry-force']['context']
        after = ev.steps['expiry-query']['context']
        moved = after.get('epoch')
        ok = (before.get('epoch') == EXPIRY_EPOCH and before.get('scene') and moved not in (None, EXPIRY_EPOCH)
              and is_reply(query, 40, 'STALE', 0, moved) and after.get('lease_deadline') == 0 and after.get('scene') == 0)
        return ok, (describe(query) + f"; ctx epoch 0x{before.get('epoch', 0):04x} -> 0x{moved or 0:04x}, "
                    f"lease {after.get('lease_deadline')}, cache block {after.get('scene', 0):#x}")
    yield 'A STATE after the lease deadline passed sees the loss: STALE with the moved epoch (revision 55)', (
        'expiry-put', 'expiry-force', 'expiry-query'), expired_query

    def present():
        answer = ev.answer('present-show')
        before, after = ev.framebuffer('idle'), ev.framebuffer('present-show')
        if after:
            framebuffer_png(after, ev.output / 'present-show.png')
        if before:
            framebuffer_png(before, ev.output / 'idle.png')
        box = lit_box(after) if after else None
        presents = len(ev.hits('present-show', TAG_PRESENT))
        near = bool(box and abs(box[0] - TEXT_X) <= 4 and abs(box[1] - TEXT_Y) <= 8
                    and 40 <= box[2] - box[0] <= 160 and 8 <= box[3] - box[1] <= 30)
        ok = is_reply(answer, 38, 'APPLIED', 12, PRESENT_EPOCH) and presents >= 1 and near
        return ok, describe(answer) + f'; cfw_scene_present calls {presents}; lit pixels only in box {box}'
    yield 'Presenting SHOW draws the cached text on the panel framebuffer', (
        'present-reset', 'present-put', 'present-show'), present

    def resent_chunk():
        first, last, resent = ev.answer('chunk-first'), ev.answer('chunk-last'), ev.answer('chunk-resent')
        revision = cfw_revision()
        expected = 'APPLIED' if revision >= 54 else 'REFUSED'
        ok = (is_reply(first, 37, 'APPLIED', 20, PRESENT_EPOCH) and is_reply(last, 37, 'APPLIED', 21, PRESENT_EPOCH)
              and is_reply(resent, 37, expected, 22, PRESENT_EPOCH))
        return ok, f'revision {revision} expects {expected}: last chunk {describe(last)}; the same chunk again {describe(resent)}'
    yield 'The last chunk of a complete asset sent again is a no-op from revision 54 and refused before', (
        'chunk-first', 'chunk-last', 'chunk-resent'), resent_chunk

    def repeated_request():
        repeat, fresh = ev.answer('repeat-id-show'), ev.answer('fresh-id-show')
        shown = lit_box(ev.framebuffer('present-show') or b'')
        after_repeat = lit_box(ev.framebuffer('repeat-id-show') or b'')
        after_fresh = lit_box(ev.framebuffer('fresh-id-show') or b'')
        ok = (is_reply(repeat, 38, 'APPLIED', 12, PRESENT_EPOCH) and shown is not None and after_repeat == shown
              and is_reply(fresh, 38, 'APPLIED', 13, PRESENT_EPOCH) and after_fresh is None)
        return ok, (f'same id: {describe(repeat)}, lit box {shown} -> {after_repeat}; '
                    f'new id: {describe(fresh)}, lit box -> {after_fresh}')
    yield 'A different SHOW reusing the last applied request id is acknowledged and not applied', (
        'present-show', 'repeat-id-show', 'fresh-id-show'), repeated_request


def evaluate(ev):
    verdicts = []
    for name, needs, check in scenario_checks(ev):
        if not ev.ran(*needs):
            missing = [n for n in needs if n not in ev.steps]
            verdicts.append((name, 'INCONCLUSIVE', f'steps not reached: {missing or needs}'))
            continue
        passed, detail = check()
        verdicts.append((name, 'VERIFIED' if passed else 'NOT VERIFIED', detail))
    return verdicts


def outside_git(path):
    return not any((parent / '.git').exists() for parent in [path, *path.parents])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, default=REPO / 'g2_2.3.0.24_cfw.bin')
    parser.add_argument('--blackwall', type=Path, default=Path.home() / 'dev/blackwall')
    parser.add_argument('--runs', type=Path, default=Path.home() / 'dev/blackwall-runs')
    parser.add_argument('--evaluate', type=Path, help='re-evaluate an existing run directory without running Renode')
    args = parser.parse_args()

    if args.evaluate:
        output = args.evaluate.resolve()
        meta = json.loads((output / 'runner.json').read_text())
    else:
        image = args.image.read_bytes()
        body = main_body(image)
        symbols, base = cfw_symbols(body)
        plan = build_plan(symbols, RIGHT_LENS, context_layout())
        output = (args.runs / time.strftime('cache-%Y%m%d-%H%M%S')).resolve()
        if not outside_git(output.parent):
            raise SystemExit('--runs must be outside every git repository (the run copies firmware)')
        stage(output, args.blackwall, body, plan)
        meta = dict(image_sha256=sha256(image), blob_base=base)
        (output / 'runner.json').write_text(json.dumps(meta, indent=1))
        print(f'run directory {output}')
        started = time.time()
        code = run_renode(output, timeout=1200)
        print(f'renode exit {code} after {time.time() - started:.0f} s')

    result_path = output / 'exports/cache-result.json'
    if not result_path.exists():
        log = (output / 'renode.log').read_text(errors='replace')
        errors = [line for line in log.splitlines() if 'ERROR' in line][-5:]
        print('FAIL harness: no cache-result.json; ' + (' | '.join(errors) or 'see renode.log'))
        return 1
    boot_path = output / 'exports/family-boot.json'
    if boot_path.exists():
        boot = json.loads(boot_path.read_text())
        print(f"boot: stock frame {boot.get('frame')} serial {boot.get('frame_serial')}, {boot.get('nonzero')} lit pixels, "
              f"{boot.get('instructions')} instructions")
    report = json.loads(result_path.read_text())
    for step in report['results']:
        borrowed = ', '.join(f"{p.get('task') or '?'} after {p.get('borrow_wait_ms')} ms -> status {p.get('status', p.get('error'))}"
                             for p in step.get('packets', []))
        ctx = step.get('context', {})
        print(f"  {step['name']}: os tick {step['start_os_tick']}->{step['end_os_tick']}, wall {step['wall_seconds']} s, "
              f"ctx epoch {ctx.get('epoch')} lease {ctx.get('lease_deadline')} lost {ctx.get('cache_lost')}"
              + (f'; {borrowed}' if borrowed else '')
              + (f"; peer uart tx {len(step.get('uart_tx', []))} acked {sum(a['accepted'] for a in step.get('peer_acks', []))}"
                 if step.get('uart_tx') else ''))
    used = report['results'][-1]['end_os_tick'] - report['results'][0]['start_os_tick']
    print(f'  virtual time used after the first frame: {used} ms of about {FIRST_FRAME_BUDGET_MS} ms before the board model stops')
    if report.get('aborted'):
        print(f"  machine stopped during {report['aborted']['step']}: {report['aborted']}")
    ev = Evidence(report, output)
    tasks = {}
    for step in report['results']:
        for row in step['rows']:
            tasks.setdefault(row['tag'], set()).add(row.get('task') or hex(row['tcb']))
    print('  tasks: ' + '; '.join(f"{name} on {', '.join(sorted(tasks[tag]))}" for tag, name, *_ in WATCHES
                                  if tag in tasks and tag not in (TAG_SEND, TAG_BRIDGE)))
    failed = 0
    for name, outcome, detail in evaluate(ev):
        print(f'{"PASS" if outcome == "VERIFIED" else "FAIL"} [{outcome}] {name}: {detail}')
        failed += outcome != 'VERIFIED'
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
