// Headless post-script: per function, the features a match between two builds can lean on, one JSON
// object per line in <out file>: entry, names, size, blocks, mnemonic hash, ordered calls (entries;
// imports as "EXT:name"), referenced strings, large constants.
// Usage: ghidra-analyzeHeadless ... -postScript ExportFeatures.java <out.jsonl>
//@category ChaosRings

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.block.BasicBlockModel;
import ghidra.program.model.block.CodeBlockIterator;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class ExportFeatures extends GhidraScript {
    static String json(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char ch : s.toCharArray()) {
            if (ch == '"' || ch == '\\') b.append('\\').append(ch);
            else if (ch < 0x20 || ch > 0x7e) b.append(String.format("\\u%04x", (int) ch));
            else b.append(ch);
        }
        return b.append('"').toString();
    }

    static String list(List<String> xs) {
        return "[" + String.join(",", xs) + "]";
    }

    // A string at a, or behind a pointer at a (CFString / literal pool), else null
    String stringAt(Address a, int depth) {
        Data d = getDataAt(a);
        if (d == null) d = getDataContaining(a);
        if (d == null) return null;
        Object v = d.getValue();
        if (v instanceof String) return (String) v;
        if (depth > 0 && d.isPointer() && v instanceof Address) return stringAt((Address) v, depth - 1);
        if (depth > 0 && d.isStructure()) { // CFString: {isa, flags, char*, length}
            for (int i = 0; i < d.getNumComponents(); ++i) {
                Data c = d.getComponent(i);
                if (c.isPointer() && c.getValue() instanceof Address) {
                    String s = stringAt((Address) c.getValue(), 0);
                    if (s != null) return s;
                }
            }
        }
        return null;
    }

    // Ghidra externals, and the Vita's import stubs (functions in a Sce* library namespace,
    // VitaSetup.java), count as imports
    static boolean isImport(Function g) {
        return g.isExternal() || g.getParentNamespace().getName().startsWith("Sce");
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        Memory mem = currentProgram.getMemory();
        BasicBlockModel bbm = new BasicBlockModel(currentProgram);
        int n = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                List<String> names = new ArrayList<>();
                names.add(json(f.getName(true)));
                for (Symbol s : currentProgram.getSymbolTable().getSymbols(f.getEntryPoint()))
                    if (!s.getName(true).equals(f.getName(true))) names.add(json(s.getName(true)));
                List<String> calls = new ArrayList<>(), strings = new ArrayList<>(), consts = new ArrayList<>();
                java.util.Map<String, Integer> movwAt = new java.util.HashMap<>();
                java.util.Map<String, Long> movwLo = new java.util.HashMap<>();
                StringBuilder mn = new StringBuilder();
                int ninstr = 0;
                InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (ii.hasNext()) {
                    Instruction ins = ii.next();
                    ++ninstr;
                    mn.append(ins.getMnemonicString()).append(' ');
                    for (Reference r : ins.getReferencesFrom()) {
                        Address to = r.getToAddress();
                        Function g = getFunctionAt(to);
                        if (r.getReferenceType().isCall() || (g != null && r.getReferenceType().isData())) {
                            if (g == null) g = getFunctionContaining(to);
                            if (g == null) continue;
                            if (g.isThunk() && g.getThunkedFunction(true) != null) g = g.getThunkedFunction(true);
                            calls.add(json(isImport(g) ? "EXT:" + g.getName() : g.getEntryPoint().toString()));
                            continue;
                        }
                        if (r.getReferenceType().isFlow() || !to.isMemoryAddress() || !mem.contains(to)) continue;
                        String s = stringAt(to, 2);
                        if (s != null) { strings.add(json(s)); continue; }
                        if (f.getBody().contains(to) && getInstructionAt(to) == null) { // a literal pool word
                            try {
                                long v = mem.getInt(to) & 0xffffffffL;
                                Address va = to.getNewAddress(v);
                                if (mem.contains(va)) {
                                    Function h = getFunctionAt(va);
                                    if (h != null) { calls.add(json("ADDR:" + h.getEntryPoint())); continue; }
                                    String s2 = stringAt(va, 1);
                                    if (s2 != null) strings.add(json(s2));
                                } else if (v > 0xff) {
                                    // a PC-relative offset (ldr rX, [pc, #]; add rX, pc, rX) lands in
                                    // another section from here: it changes with the layout, skip it
                                    Address rel = v < 0x1000000L ? to.add(v) : null;
                                    if (rel == null || mem.getBlock(rel) == null || mem.getBlock(rel) == mem.getBlock(to))
                                        consts.add(Long.toString(v));
                                }
                            } catch (Exception e) {
                                // not readable
                            }
                        }
                    }
                    // movw rX, #lo / movt rX, #hi building an address: neither half is a constant
                    String mnem = ins.getMnemonicString().split("\\.")[0];
                    if ((mnem.equals("movt") || mnem.equals("movw")) && ins.getNumOperands() == 2) {
                        String reg = ins.getDefaultOperandRepresentation(0);
                        Object[] imm = ins.getOpObjects(1);
                        long v = imm.length == 1 && imm[0] instanceof Scalar ? ((Scalar) imm[0]).getUnsignedValue() : -1;
                        if (mnem.equals("movw") && v >= 0) {
                            movwAt.put(reg, consts.size());
                            movwLo.put(reg, v);
                            consts.add(Long.toString(v));
                            continue;
                        }
                        if (mnem.equals("movt") && v >= 0) {
                            Integer at = movwAt.remove(reg);
                            long lo = at != null ? movwLo.remove(reg) : 0;
                            if (mem.contains(toAddr((v << 16) | lo))) {
                                if (at != null) consts.set(at, null);
                                continue;
                            }
                        }
                    }
                    for (int op = 0; op < ins.getNumOperands(); ++op)
                        for (Object o : ins.getOpObjects(op))
                            if (o instanceof Scalar) {
                                long v = ((Scalar) o).getUnsignedValue();
                                if (v > 0xff && v < 0x10000000L && !mem.contains(toAddr(v))) // (not an address)
                                    consts.add(Long.toString(v));
                            }
                }
                consts.removeIf(c -> c == null || Long.parseLong(c) <= 0xff);
                int blocks = 0;
                CodeBlockIterator bi = bbm.getCodeBlocksContaining(f.getBody(), monitor);
                while (bi.hasNext()) { bi.next(); ++blocks; }
                w.printf("{\"entry\":\"%s\",\"names\":%s,\"thunk\":%b,\"external\":%b,\"size\":%d,\"ninstr\":%d,"
                         + "\"blocks\":%d,\"mn\":%s,\"calls\":%s,\"strings\":%s,\"consts\":[%s]}%n",
                         f.getEntryPoint(), list(names), f.isThunk(), isImport(f), f.getBody().getNumAddresses(),
                         ninstr, blocks, json(mn.toString().trim()), list(calls), list(strings), String.join(",", consts));
                ++n;
            }
        }
        println("ExportFeatures: " + n + " functions -> " + out);
    }
}
