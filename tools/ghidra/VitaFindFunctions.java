// Headless pre-script for a second analysis pass over a Vita ELF: functions only reached through
// pointers (vtables, callback tables) - every odd (Thumb) word in the loaded segments that points
// into the code where no function is yet and lands on a prologue (push / stmdb sp! / sub sp) - and
// Ghidra's ARM Aggressive Instruction Finder on for the rest.
// Usage: ghidra-analyzeHeadless ... -process <program> -preScript VitaFindFunctions.java
//@category ChaosRings

import java.math.BigInteger;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class VitaFindFunctions extends GhidraScript {
    boolean prologue(Address a) throws Exception {
        int h = currentProgram.getMemory().getShort(a) & 0xffff;
        return (h & 0xfe00) == 0xb400        // push {..} / push {.., lr}
            || h == 0xe92d                   // push.w / stmdb sp!, {..}
            || (h & 0xff80) == 0xb080;       // sub sp, #n
    }

    @Override
    public void run() throws Exception {
        setAnalysisOption(currentProgram, "ARM Aggressive Instruction Finder", "true");
        Memory mem = currentProgram.getMemory();
        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        MemoryBlock code = null;
        for (MemoryBlock b : mem.getBlocks())
            if (b.isExecute()) code = b;
        TreeSet<Address> found = new TreeSet<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized()) continue;
            for (Address a = b.getStart(); a.compareTo(b.getEnd().subtract(3)) <= 0; a = a.add(4)) {
                long v = mem.getInt(a) & 0xffffffffL;
                if ((v & 1) == 0) continue;
                Address t = toAddr(v & ~1L);
                if (!code.contains(t) || getFunctionContaining(t) != null || getInstructionContaining(t) != null)
                    continue;
                if (prologue(t)) found.add(t);
            }
        }
        int made = 0;
        for (Address t : found) {
            if (getFunctionContaining(t) != null) continue;
            currentProgram.getProgramContext().setValue(tmode, t, t, BigInteger.ONE);
            if (disassemble(t) && createFunction(t, null) != null) ++made;
        }
        println(String.format("VitaFindFunctions: %d pointer targets on a prologue, %d functions made; aggressive finder on",
                              found.size(), made));
    }
}
