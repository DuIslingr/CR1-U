// Headless post-script: names functions from match_functions.py's table (entry, other build's entry,
// name, mangled name, how, score). C++ names are demangled onto the function (namespace, signature);
// a plate comment says where the name came from and how sure the match is.
// Usage: ghidra-analyzeHeadless ... -postScript ApplyNames.java <names.tsv> <source label>
// (a 7th "source" column, as combine_names.py writes, replaces the label per row)
//@category ChaosRings

import java.io.BufferedReader;
import java.io.FileReader;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.demangler.DemangledObject;
import ghidra.app.util.demangler.DemanglerOptions;
import ghidra.app.util.demangler.DemanglerUtil;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

public class ApplyNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String source = args.length > 1 ? args[1] : "the other build";
        SymbolTable st = currentProgram.getSymbolTable();
        DemanglerOptions dopts = new DemanglerOptions();
        int named = 0, demangled = 0, skipped = 0, low = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
            String line = r.readLine(); // header
            while ((line = r.readLine()) != null) {
                String[] c = line.split("\t", -1);
                Address at = toAddr(c[0]);
                Function f = getFunctionAt(at);
                if (f == null) { ++skipped; continue; }
                String name = c[2], mangled = c[3], how = c[4];
                double score = Double.parseDouble(c[5]);
                boolean sure = how.startsWith("agreeing")
                    || (!how.equals("call position") && !how.startsWith("disputed")
                        && (score >= 0.5 || how.equals("name") || how.equals("strings")));
                if (!sure) ++low;
                String from = c.length > 6 && !c[6].isEmpty() ? c[6] // combine_names.py: builds and entries
                    : source + " @ 0x" + c[1].replaceFirst("^0+", "");
                setPlateComment(at, String.format("Name from %s: matched by %s, score %.2f%s", from, how, score,
                                                  sure ? "" : " (low confidence: check before relying on it)"));
                if (how.equals("name")) { ++named; continue; } // already has it
                boolean done = false;
                if (!mangled.isEmpty()) {
                    String m = mangled.startsWith("__Z") ? mangled.substring(1) : mangled; // Mach-O's extra _
                    try {
                        List<DemangledObject> ds = DemanglerUtil.demangle(currentProgram, m, at);
                        if (ds != null && !ds.isEmpty() && ds.get(0).applyTo(currentProgram, at, dopts, monitor)) {
                            done = true;
                            ++demangled;
                        }
                    } catch (Exception e) {
                        println("demangle " + mangled + ": " + e.getMessage());
                    }
                }
                if (!done) { // a plain name, maybe qualified (Class::method)
                    String[] parts = name.split("::");
                    Namespace ns = currentProgram.getGlobalNamespace();
                    for (int i = 0; i < parts.length - 1; ++i)
                        ns = st.getOrCreateNameSpace(ns, parts[i], SourceType.IMPORTED);
                    f.getSymbol().setNameAndNamespace(parts[parts.length - 1], ns, SourceType.IMPORTED);
                }
                ++named;
            }
        }
        println(String.format("ApplyNames: %d named (%d demangled), %d low confidence, %d not found", named,
                              demangled, low, skipped));
    }
}
