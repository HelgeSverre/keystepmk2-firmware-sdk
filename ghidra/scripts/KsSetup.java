// Pre-analysis setup for the KeyStep mk2 application image (STM32G0B1-class, base 0x08008000).
// Adds RAM and peripheral blocks, types the vector table and seeds functions from it.
//@category KeyStep
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class KsSetup extends GhidraScript {
    private void block(String name, long start, long size, boolean exec) throws Exception {
        Address a = toAddr(start);
        if (currentProgram.getMemory().getBlock(a) != null) return;
        MemoryBlock b = currentProgram.getMemory().createUninitializedBlock(name, a, size, false);
        b.setRead(true); b.setWrite(true); b.setExecute(exec); b.setVolatile(start >= 0x40000000L);
    }

    @Override
    public void run() throws Exception {
        long baseAddr = 0x08008000L;
        block("SRAM", 0x20000000L, 0x24000, false);       // initial SP is 0x20024000
        block("BOOTLOADER", 0x08000000L, 0x8000, true);   // not in the package
        block("SYSMEM", 0x1FFF0000L, 0x8000, false);
        block("APB", 0x40000000L, 0x16000, false);
        block("AHB", 0x40020000L, 0x6400, false);
        block("IOPORT", 0x50000000L, 0x2000, false);
        block("PPB", 0xE0000000L, 0x100000, false);
        MemoryBlock app = currentProgram.getMemory().getBlock(toAddr(baseAddr));
        app.setName("APP"); app.setWrite(false);

        String[] exc = {"InitialSP", "Reset", "NMI", "HardFault", null, null, null,
            null, null, null, null, "SVC", null, null, "PendSV", "SysTick"};
        long end = app.getEnd().getOffset();
        Address base = toAddr(baseAddr);
        for (int i = 0; i < 48; i++) {                    // Cortex-M0+: 16 system + 32 interrupts
            Address slot = base.add(4L * i);
            clearListing(slot, slot.add(3));
            createData(slot, PointerDataType.dataType);
            long v = getInt(slot) & 0xFFFFFFFFL;
            String n = i < 16 ? exc[i] : "IRQ" + (i - 16);
            if (n == null) continue;
            createLabel(slot, "VEC_" + n, true, SourceType.USER_DEFINED);
            if (i == 0 || v < baseAddr || v > end) continue;
            Address f = toAddr(v & ~1L);
            disassemble(f);
            if (getFunctionAt(f) == null) createFunction(f, null);
            if (getFunctionAt(f) != null && getFunctionAt(f).getSymbol().getSource() == SourceType.DEFAULT)
                getFunctionAt(f).setName(n + "_Handler", SourceType.USER_DEFINED);
        }
    }
}
