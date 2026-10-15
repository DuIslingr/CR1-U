// Headless pre-script for a PS Vita ELF from tools/vita_self.py: the code segment defaults to Thumb
// (SNC builds are Thumb-2), the import stubs (16 bytes, ARM) get their library function names, and
// exports (module_start, ...) and the module info are marked, before auto-analysis runs.
// Usage: ghidra-analyzeHeadless ... -import eboot.elf -preScript VitaSetup.java <imports.tsv>
//@category ChaosRings

import java.io.BufferedReader;
import java.io.FileReader;
import java.math.BigInteger;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.ProgramContext;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

public class VitaSetup extends GhidraScript {
    @Override
    public void run() throws Exception {
        ProgramContext ctx = currentProgram.getProgramContext();
        Register tmode = ctx.getRegister("TMode");
        for (MemoryBlock b : currentProgram.getMemory().getBlocks())
            if (b.isExecute())
                ctx.setValue(tmode, b.getStart(), b.getEnd(), BigInteger.ONE);
        int imports = 0, exports = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
            r.readLine();
            String line;
            while ((line = r.readLine()) != null) {
                String[] c = line.split("\t", -1);
                long a = Long.parseLong(c[0], 16);
                String name = c[1], kind = c[2], lib = c[3], nid = c[4];
                if (kind.equals("import")) {
                    Address at = toAddr(a);
                    ctx.setValue(tmode, at, at.add(15), BigInteger.ZERO);
                    new DisassembleCommand(at, new AddressSet(at, at.add(15)), true).applyTo(currentProgram);
                    Function f = getFunctionAt(at);
                    if (f == null) f = createFunction(at, name);
                    Namespace ns = currentProgram.getSymbolTable().getOrCreateNameSpace(
                        currentProgram.getGlobalNamespace(), lib, SourceType.IMPORTED);
                    f.getSymbol().setNameAndNamespace(name, ns, SourceType.IMPORTED);
                    setPlateComment(at, "Import stub: " + lib + " NID 0x" + nid + " (the loader patches it)");
                    ++imports;
                } else if (kind.equals("export")) {
                    boolean thumb = (a & 1) != 0;
                    Address at = toAddr(a & ~1L);
                    ctx.setValue(tmode, at, at, thumb ? BigInteger.ONE : BigInteger.ZERO);
                    disassemble(at);
                    Function f = getFunctionAt(at);
                    if (f == null) f = createFunction(at, name);
                    else f.setName(name, SourceType.IMPORTED);
                    addEntryPoint(at);
                    ++exports;
                } else {
                    createLabel(toAddr(a), name, true, SourceType.IMPORTED);
                }
            }
        }
        println(String.format("VitaSetup: Thumb by default, %d import stubs, %d exports", imports, exports));
    }
}
