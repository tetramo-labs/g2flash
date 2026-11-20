try:
    import clr, json, time
    from System import Console
    from System.IO import File
    from Antmicro.Renode.Utilities import TypeManager
    from Antmicro.Renode.Core import EmulationManager
    from Antmicro.Renode.Time import TimeInterval
    clr.AddReference(TypeManager.Instance.GetTypeByName('Antmicro.Renode.Peripherals.Miscellaneous.GlasslyCacheProbe').Assembly)
    from Antmicro.Renode.Peripherals.Miscellaneous import GlasslyCacheProbe

    plan = json.loads(File.ReadAllText('/work/cache-plan.json'))
    m = monitor.Machine
    bus = m.SystemBus
    cpu = m['sysbus.cpu']
    emulation = EmulationManager.Instance.CurrentEmulation
    ticks_per_ms = int(TimeInterval.TicksPerMillisecond)
    if not m.IsPaused:
        raise ValueError('Boot profile did not leave the machine paused')

    probe = GlasslyCacheProbe(m, cpu, plan['tcb_slot'])
    for watch in plan['watches']:
        probe.Watch(watch['tag'], watch['pc'], watch['data_register'], watch['length_register'], watch['max_length'])
    probe.Borrow(plan['borrow_pc'])

    def now_ticks():
        return int(m.ElapsedVirtualTime.TimeElapsed.Ticks)

    def os_tick():
        return int(bus.ReadDoubleWord(plan['os_tick']))

    def run_until(done, virtual_ms, wall_seconds):
        start = now_ticks()
        deadline = time.time() + wall_seconds
        emulation.StartAll()
        while not done() and not m.IsPaused and time.time() < deadline and now_ticks() - start < virtual_ms * ticks_per_ms:
            time.sleep(0.002)
        m.Pause()
        return (now_ticks() - start) // ticks_per_ms

    def task_name(tcb):
        if tcb < 0x20000000 or tcb >= 0x20380000:
            return ''
        raw = [int(b) for b in bus.ReadBytes(tcb + plan['tcb_name_offset'], 16, True)]
        text = ''
        for b in raw:
            if b == 0:
                break
            text += chr(b) if 32 <= b < 127 else '?'
        return text

    def write_scratch(data):
        address = plan['scratch']
        for i in range(len(data)):
            bus.WriteByte(address + i, data[i])
        return address

    def inject(packet):
        record = dict(packet=packet)
        probe.Arm()
        waited = run_until(lambda: probe.State == GlasslyCacheProbe.Borrowed, plan['borrow_wait_ms'], 300)
        record['borrow_wait_ms'] = waited
        if probe.State != GlasslyCacheProbe.Borrowed:
            probe.Disarm()
            record['error'] = 'task never reached the borrow point'
            return record
        record['tcb'] = int(probe.BorrowedTcb)
        record['task'] = task_name(int(probe.BorrowedTcb))
        data = [int(packet[i:i + 2], 16) for i in range(0, len(packet), 2)]
        address = write_scratch(data)
        record['os_tick'] = os_tick()
        probe.Call(plan['entry'], 0, address, len(data), 0)
        elapsed = run_until(lambda: probe.State == GlasslyCacheProbe.Returned, plan['call_wait_ms'], 300)
        record['call_ms'] = elapsed
        if probe.State != GlasslyCacheProbe.Returned:
            record['error'] = 'injected call did not return'
            return record
        record['status'] = int(probe.Restore())
        return record

    def context():
        ctx = int(bus.ReadDoubleWord(plan['ctx_slot']))
        layout = plan['ctx_layout']
        if ctx < 0x20000000 or ctx >= 0x20380000 or int(bus.ReadDoubleWord(ctx)) != plan['ctx_magic']:
            return dict(pointer=ctx, valid=False)
        return dict(pointer=ctx, valid=True,
                    lease_deadline=int(bus.ReadDoubleWord(ctx + layout['direct_lease_deadline'])),
                    epoch=int(bus.ReadWord(ctx + layout['cache_epoch'])),
                    scene=int(bus.ReadDoubleWord(ctx + layout['scene'])),
                    store=int(bus.ReadDoubleWord(ctx + layout['texture_cache'])),
                    cache_lost=int(bus.ReadByte(ctx + layout['cache_lost'])),
                    direct_active=int(bus.ReadByte(ctx + layout['direct_active'])),
                    gate_timeouts=int(bus.ReadDoubleWord(ctx + layout['gate_timeouts'])))

    def expire_lease():
        state = context()
        if not state['valid'] or state['lease_deadline'] == 0:
            return dict(error='no lease to expire', context=state)
        bus.WriteDoubleWord(state['pointer'] + plan['ctx_layout']['direct_lease_deadline'], os_tick())
        return dict(deadline_before=state['lease_deadline'], deadline_after=os_tick())

    uart = m['sysbus.guard'].Uarts[0]
    uart_seen = [int(uart.DmaTx.Transfers.Count)]

    def peer_crc(values):
        crc = 0
        for value in values:
            crc ^= value
            for unused in range(8):
                crc = (crc >> 1) ^ (0xa001 if crc & 1 else 0)
        return crc

    def uart_frames():
        transfers = list(uart.DmaTx.Transfers)
        fresh = [[int(b) for b in t.Data] for t in transfers[uart_seen[0]:]]
        uart_seen[0] = len(transfers)
        return fresh

    def ack_peer(frames):
        from System import Array, Byte
        acked = []
        for data in frames:
            if len(data) < 3 or data[0] != 1:
                continue
            header = [1, data[1], data[2], 0, 1, 0, 0]
            h = peer_crc(header)
            t = peer_crc([10])
            frame = header + [h >> 8, h & 255, 10, t >> 8, t & 255]
            acked.append(dict(frame=''.join('%02x' % b for b in frame),
                              accepted=bool(uart.TryReceive(Array[Byte](frame), m['sysbus.guard'].UartPowers[0].Phase == 'on'))))
        return acked

    def framebuffer(name):
        pointer = int(bus.ReadDoubleWord(plan['framebuffer_slot']))
        size = plan['framebuffer_bytes']
        if pointer < 0x20000000 or pointer > 0x20380000 - size:
            return dict(pointer=pointer, error='framebuffer pointer outside SRAM')
        File.WriteAllBytes('/work-output/fb-' + name + '.bin', bus.ReadBytes(pointer, size, True))
        return dict(pointer=pointer, file='fb-' + name + '.bin')

    guard_names = ('rstgen', 'guard', 'wdt', 'guard_high', 'ppb_guard', 'iom2', 'iom4', 'iom5', 'iom6', 'iom7',
                   'guard_iom2_tail', 'guard_iom4_tail', 'guard_iom5_tail', 'guard_iom6_tail', 'guard_iom7_tail',
                   'xip_guard', 'guard_mram_tail')

    def stops():
        found = []
        for name in guard_names:
            node = m['sysbus.' + name]
            if int(node.AccessCount):
                found.append(dict(name=name, address=int(node.LastAddress), operation=str(node.LastOperation),
                                  value=int(node.LastValue)))
        policy = globals().get('memory_policy')
        if policy is not None and int(policy.Violations):
            found.append(dict(name='memory_policy', address=int(policy.LastAddress), operation=str(policy.LastOperation),
                              pc=int(policy.LastPC)))
        control = globals().get('system_control')
        if control is not None and int(control.Violations):
            found.append(dict(name='system_control', address=int(control.LastAddress), operation=str(control.LastOperation),
                              pc=int(control.LastPC)))
        return found

    def cpu_state():
        tcb = int(bus.ReadDoubleWord(plan['tcb_slot']))
        return dict(pc=int(cpu.PC.RawValue), lr=int(cpu.GetRegister(14).RawValue), sp=int(cpu.GetRegister(13).RawValue),
                    xpsr=int(cpu.GetRegister(25).RawValue), tcb=tcb, task=task_name(tcb))

    baseline_stops = stops()
    results = []
    aborted = None
    for step in plan['steps']:
        if aborted:
            break
        first_row = probe.Rows.Count
        entry = dict(name=step['name'], op=step['op'], start_os_tick=os_tick())
        wall = time.time()
        if step['op'] == 'inject':
            entry['packets'] = []
            for packet in step['packets']:
                record = inject(packet)
                entry['packets'].append(record)
                if 'error' in record:
                    break
            if any('error' in record for record in entry['packets']):
                aborted = dict(step=step['name'], stops=stops(), cpu=cpu_state(), error=entry['packets'][-1]['error'])
            elif step.get('ack_peer'):
                entry['uart_tx'] = []
                entry['peer_acks'] = []
                settled = 0
                while settled < step.get('settle_ms', 0):
                    settled += max(1, run_until(lambda: False, 5, 60))
                    frames = uart_frames()
                    entry['uart_tx'] += [''.join('%02x' % b for b in f) for f in frames]
                    entry['peer_acks'] += ack_peer(frames)
                entry['settle_ms'] = settled
            elif step.get('settle_ms', 0):
                entry['settle_ms'] = run_until(lambda: False, step['settle_ms'], 600)
        elif step['op'] == 'run':
            entry['ran_ms'] = run_until(lambda: False, step['ms'], 1800)
        elif step['op'] == 'expire':
            entry['expire'] = expire_lease()
        if 'uart_tx' not in entry:
            entry['uart_tx'] = [''.join('%02x' % b for b in f) for f in uart_frames()]
        if step.get('framebuffer'):
            entry['framebuffer'] = framebuffer(step['name'])
        entry['context'] = context()
        entry['borrow_hits'] = int(probe.Hits)
        entry['end_os_tick'] = os_tick()
        entry['wall_seconds'] = round(time.time() - wall, 2)
        rows = []
        for i in range(first_row, probe.Rows.Count):
            row = [int(v) for v in probe.Rows[i]]
            rows.append(dict(tag=row[0], pc=row[1], r=row[2:6], sp=row[6], lr=row[7], tcb=row[8], xpsr=row[9],
                             ticks=row[10], data=''.join('%02x' % int(b) for b in probe.Payloads[i])))
        entry['rows'] = rows
        entry['cpu'] = cpu_state()
        current_stops = stops()
        if current_stops != baseline_stops and not aborted:
            aborted = dict(step=step['name'], stops=current_stops, cpu=entry['cpu'])
        results.append(entry)
        Console.WriteLine('GLASSLY_CACHE_STEP ' + json.dumps(dict(name=entry['name'], rows=len(rows), wall=entry['wall_seconds'])))
        Console.Out.Flush()

    report = dict(results=results, overflow=bool(probe.Overflow), ticks_per_ms=ticks_per_ms, aborted=aborted,
                  baseline_stops=baseline_stops, instructions=int(cpu.ExecutedInstructions))
    File.WriteAllText('/work-output/cache-result.json', json.dumps(report))
    Console.WriteLine('GLASSLY_CACHE_DONE')
except Exception as error:
    from System import Console
    Console.WriteLine('GLASSLY_CACHE_ERROR ' + str(error))
