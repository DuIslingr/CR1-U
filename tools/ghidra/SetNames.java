// Headless post-script: names functions by hand from a table (entry, name, plate comment). A name of
// "-" puts the default FUN_ name back and clears the plate comment. Class::name makes the namespace.
// Usage: ghidra-analyzeHeadless ... -postScript SetNames.java <names.tsv>
//@category ChaosRings

import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

public class SetNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        SymbolTable st = currentProgram.getSymbolTable();
        int named = 0, reset = 0, missing = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] c = line.split("\t", -1);
                Address at = toAddr(c[0]);
                Function f = getFunctionAt(at);
                if (f == null) { ++missing; println("no function at " + c[0]); continue; }
                if (c[1].equals("-")) {
                    f.getSymbol().setNameAndNamespace("FUN_" + at, currentProgram.getGlobalNamespace(), SourceType.DEFAULT);
                    setPlateComment(at, null);
                    ++reset;
                    continue;
                }
                String[] parts = c[1].split("::");
                Namespace ns = currentProgram.getGlobalNamespace();
                for (int i = 0; i < parts.length - 1; ++i)
                    ns = st.getOrCreateNameSpace(ns, parts[i], SourceType.USER_DEFINED);
                f.getSymbol().setNameAndNamespace(parts[parts.length - 1], ns, SourceType.USER_DEFINED);
                if (c.length > 2 && !c[2].isEmpty()) setPlateComment(at, c[2].replace("\\n", "\n"));
                ++named;
            }
        }
        println(String.format("SetNames: %d named, %d reset, %d not found", named, reset, missing));
    }
}
