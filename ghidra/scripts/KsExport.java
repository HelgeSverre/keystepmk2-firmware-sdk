// Export decompiled C, a function index and defined strings to ghidra/out/.
//@category KeyStep
import java.io.File;
import java.io.PrintWriter;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;

public class KsExport extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : ".");
        out.mkdirs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter c = new PrintWriter(new File(out, "decomp.c"));
             PrintWriter idx = new PrintWriter(new File(out, "functions.tsv"))) {
            idx.println("address\tname\tsize\tcallers");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                idx.printf("%s\t%s\t%d\t%d%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses(),
                    f.getCallingFunctions(monitor).size());
                DecompileResults r = d.decompileFunction(f, 60, monitor);
                c.printf("%n// ===== %s @ %s =====%n", f.getName(), f.getEntryPoint());
                c.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed");
            }
        }
        try (PrintWriter s = new PrintWriter(new File(out, "strings.tsv"))) {
            for (Data dt : currentProgram.getListing().getDefinedData(true)) {
                StringDataInstance si = StringDataInstance.getStringDataInstance(dt);
                if (si != null && si.getStringValue() != null && si.getStringLength() >= 4)
                    s.printf("%s\t%s%n", dt.getAddress(), si.getStringValue().replace("\n", "\\n"));
            }
        }
    }
}
