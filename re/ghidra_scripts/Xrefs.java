// Lists code references to a set of data addresses, grouped by containing function.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.util.*;

public class Xrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        for (String a : args) {
            Address addr = toAddr(a);
            Map<String, List<String>> byFunc = new TreeMap<>();
            int n = 0;
            for (Reference r : getReferencesTo(addr)) {
                Function f = getFunctionContaining(r.getFromAddress());
                String name = f == null ? "(no function)" : f.getName() + " @ " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses();
                byFunc.computeIfAbsent(name, k -> new ArrayList<>()).add(r.getFromAddress() + ":" + r.getReferenceType().getName());
                n++;
            }
            println("XREF " + a + " refs=" + n + " funcs=" + byFunc.size());
            for (Map.Entry<String, List<String>> e : byFunc.entrySet()) {
                List<String> l = e.getValue();
                println("XREF   " + e.getKey() + "  x" + l.size() + "  " + String.join(" ", l.subList(0, Math.min(4, l.size()))));
            }
        }
    }
}
