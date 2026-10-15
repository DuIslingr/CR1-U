// Headless post-script: writes every function's decompiled C to <out>/decomp.c and a list of
// functions (entry, size, name, calls out) to <out>/functions.tsv.
// Usage: ghidra-analyzeHeadless ... -postScript ExportDecomp.java <out dir>
//@category ChaosRings

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : ".");
        out.mkdirs();

        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        ifc.setOptions(opts);
        ifc.toggleCCode(true);
        ifc.toggleSyntaxTree(false);
        ifc.setSimplificationStyle("decompile");
        if (!ifc.openProgram(currentProgram)) throw new Exception("decompiler: " + ifc.getLastMessage());

        int total = currentProgram.getFunctionManager().getFunctionCount(), done = 0, failed = 0;
        try (PrintWriter c = new PrintWriter(new File(out, "decomp.c"), "UTF-8");
             PrintWriter t = new PrintWriter(new File(out, "functions.tsv"), "UTF-8")) {
            c.printf("// %s: Ghidra %s decompilation, %d functions%n%n", currentProgram.getName(),
                     getGhidraVersion(), total);
            t.println("entry\tsize\tname\tthunk\tcalls");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                StringBuilder calls = new StringBuilder();
                for (Function g : f.getCalledFunctions(monitor)) {
                    if (calls.length() > 0) calls.append(',');
                    calls.append(g.getName());
                }
                t.printf("%s\t%d\t%s\t%s\t%s%n", f.getEntryPoint(), f.getBody().getNumAddresses(), f.getName(),
                         f.isThunk() ? "thunk" : "", calls);
                if (f.isThunk() || f.isExternal()) continue;
                DecompileResults r = ifc.decompileFunction(f, 120, monitor);
                c.printf("// ---- %s @ %s%n", f.getName(), f.getEntryPoint());
                if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
                    c.println(r.getDecompiledFunction().getC());
                } else {
                    c.printf("// decompile failed: %s%n%n", r == null ? "?" : r.getErrorMessage());
                    ++failed;
                }
                if (++done % 500 == 0) println(String.format("decompiled %d / %d", done, total));
            }
        } finally {
            ifc.dispose();
        }
        println(String.format("ExportDecomp: %d functions decompiled, %d failed -> %s", done, failed, out));
    }
}
