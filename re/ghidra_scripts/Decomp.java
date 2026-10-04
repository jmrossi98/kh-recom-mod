// Decompiles the functions at the given entry addresses into re/decomp/<addr>.c
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.File;
import java.io.PrintWriter;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        out.mkdirs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            Function f = getFunctionContaining(toAddr(args[i]));
            if (f == null) { println("no function at " + args[i]); continue; }
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage();
            try (PrintWriter w = new PrintWriter(new File(out, f.getEntryPoint() + ".c"))) { w.print(c); }
            println("wrote " + f.getEntryPoint() + " (" + c.length() + " chars)");
        }
    }
}
