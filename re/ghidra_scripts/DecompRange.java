// Decompiles every function whose entry lies in [start, end) into <outdir>/<addr>.c
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.File;
import java.io.PrintWriter;

public class DecompRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        out.mkdirs();
        Address start = toAddr(args[1]), end = toAddr(args[2]);
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(start, true);
        int n = 0;
        while (it.hasNext()) {
            Function f = it.next();
            if (f.getEntryPoint().compareTo(end) >= 0) break;
            DecompileResults r = d.decompileFunction(f, 60, monitor);
            if (!r.decompileCompleted()) continue;
            try (PrintWriter w = new PrintWriter(new File(out, f.getEntryPoint() + ".c"))) {
                w.print(r.getDecompiledFunction().getC());
            }
            n++;
        }
        println("wrote " + n + " functions");
    }
}
