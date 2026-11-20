namespace Antmicro.Renode.Peripherals.Miscellaneous
{
    using System;
    using System.Collections.Generic;
    using Antmicro.Renode.Core;
    using Antmicro.Renode.Peripherals.CPU;

    /* Records CFW entry points and borrows one task at a quiescent instruction to
     * call injected code. The borrowed task returns to the same instruction with
     * the same SP, which is how the return is told apart from a fresh pass. */
    public sealed class GlasslyCacheProbe
    {
        public const int Idle = 0, Armed = 1, Borrowed = 2, Calling = 3, Returned = 4;

        public GlasslyCacheProbe(IMachine machine, CortexM cpu, uint tcbSlot)
        {
            this.machine = machine;
            this.cpu = cpu;
            this.tcbSlot = tcbSlot;
        }

        public void Watch(uint tag, uint pc, int dataRegister, int lengthRegister, uint maxLength)
        {
            cpu.AddHook(pc, (c, a) =>
            {
                if(Rows.Count >= 4096) { Overflow = true; return; }
                var row = Snapshot(tag, (uint)a);
                byte[] data = new byte[0];
                if(dataRegister >= 0)
                {
                    uint address = Register(dataRegister);
                    uint length = lengthRegister >= 0 ? Register(lengthRegister) : maxLength;
                    if(length > maxLength) length = maxLength;
                    if(InRam(address, length)) data = machine.SystemBus.ReadBytes(address, (int)length, true);
                }
                Rows.Add(row);
                Payloads.Add(data);
            });
        }

        public void Borrow(uint pc)
        {
            borrowPc = pc;
            cpu.AddHook(pc, (c, a) =>
            {
                Hits++;
                uint sp = Register(13);
                if(State == Armed && ((Register(25) & 0x1ffu) == 0))
                {
                    State = Borrowed;
                    BorrowedTcb = machine.SystemBus.ReadDoubleWord(tcbSlot);
                    machine.Pause();
                }
                else if(State == Calling && sp == ReturnSp)
                {
                    State = Returned;
                    machine.Pause();
                }
            });
        }

        public void Arm()
        {
            if(State != Idle) throw new InvalidOperationException("Probe busy");
            State = Armed;
        }

        public void Disarm()
        {
            if(State == Armed) State = Idle;
        }

        public void Call(uint function, uint r0, uint r1, uint r2, uint r3)
        {
            if(State != Borrowed || (uint)cpu.PC.RawValue != borrowPc) throw new InvalidOperationException("Task not borrowed");
            saved = new uint[17];
            for(int i = 0; i < 16; i++) saved[i] = Register(i);
            saved[16] = Register(25);
            ReturnSp = saved[13] & ~7u;
            cpu.SetRegister(0, r0);
            cpu.SetRegister(1, r1);
            cpu.SetRegister(2, r2);
            cpu.SetRegister(3, r3);
            cpu.SetRegister(13, ReturnSp);
            cpu.SetRegister(14, borrowPc | 1u);
            cpu.PC = function & ~1u;
            State = Calling;
        }

        public uint Restore()
        {
            if(State != Returned || (uint)cpu.PC.RawValue != borrowPc) throw new InvalidOperationException("Call has not returned");
            uint result = Register(0);
            for(int i = 0; i < 15; i++) cpu.SetRegister(i, saved[i]);
            cpu.SetRegister(25, saved[16]);
            cpu.PC = saved[15];
            State = Idle;
            return result;
        }

        private ulong[] Snapshot(uint tag, uint pc)
        {
            return new ulong[] { tag, pc, Register(0), Register(1), Register(2), Register(3), Register(13), Register(14),
                machine.SystemBus.ReadDoubleWord(tcbSlot), Register(25), machine.ElapsedVirtualTime.TimeElapsed.Ticks };
        }

        private uint Register(int index) { return (uint)cpu.GetRegister(index).RawValue; }

        private static bool InRam(uint address, uint length)
        {
            return (address >= 0x20000000u && address <= 0x20380000u - length) ||
                (address >= 0x21000000u && address <= 0x21010000u - length);
        }

        public int State { get; private set; }
        public uint Hits { get; private set; }
        public uint BorrowedTcb { get; private set; }
        public uint ReturnSp { get; private set; }
        public bool Overflow { get; private set; }
        public List<ulong[]> Rows { get; } = new List<ulong[]>();
        public List<byte[]> Payloads { get; } = new List<byte[]>();

        private readonly IMachine machine;
        private readonly CortexM cpu;
        private readonly uint tcbSlot;
        private uint borrowPc;
        private uint[] saved;
    }
}
