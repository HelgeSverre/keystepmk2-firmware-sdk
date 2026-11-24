// Second pass: find functions the auto-analysis missed. C++ reaches most code through vtables and
// callback tables, so (1) every aligned word that points at a Thumb address in the image with a
// plausible prologue becomes a function, and (2) undefined bytes that start with `push {…, lr}`
// do too. Repeats until nothing new turns up, then re-runs analysis.
//@category KeyStep
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class KsSeed extends GhidraScript {
    private boolean prologue(Memory mem, Address a) throws Exception {
        int w = mem.getShort(a) & 0xFFFF;
        return (w & 0xFF00) == 0xB500          // push {…, lr}
            || (w & 0xFF00) == 0xB400;         // push {…} (followed by another push)
    }

    private boolean make(Address f) throws Exception {
        if (getFunctionContaining(f) != null) return false;
        if (getInstructionContaining(f) != null && getInstructionAt(f) == null) return false;
        disassemble(f);
        if (getInstructionAt(f) == null) return false;
        return createFunction(f, null) != null;
    }

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock app = mem.getBlock("APP");
        long lo = app.getStart().getOffset(), hi = app.getEnd().getOffset();
        int total = 0;
        for (int round = 0; round < 6; round++) {
            int made = 0;
            for (long o = lo; o + 3 <= hi; o += 4) {                 // pointers to Thumb code
                Address slot = toAddr(o);
                if (getInstructionContaining(slot) != null) continue;
                long v = mem.getInt(slot) & 0xFFFFFFFFL;
                if ((v & 1) == 0 || v < lo + 0xC0 || v > hi) continue;
                Address f = toAddr(v & ~1L);
                if (prologue(mem, f) && make(f)) made++;
            }
            for (long o = lo + 0xC0; o + 1 <= hi; o += 2) {          // bare prologues in undefined bytes
                Address a = toAddr(o);
                if (getInstructionContaining(a) != null || getDataContaining(a) != null && getDataContaining(a).isDefined()) continue;
                if ((mem.getShort(a) & 0xFF00) == 0xB500 && make(a)) made++;
            }
            println("round " + round + ": " + made + " functions");
            total += made;
            if (made == 0) break;
            analyzeAll(currentProgram);
        }
        println("seeded " + total + " functions; now " + currentProgram.getFunctionManager().getFunctionCount());
    }
}
