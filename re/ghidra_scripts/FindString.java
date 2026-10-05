// Lists defined strings containing any of the given texts, with the functions that reference them.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class FindString extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) {
            Data d = it.next();
            if (!StringDataInstance.isString(d)) continue;
            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
            if (s == null) continue;
            for (String a : args) {
                if (!s.contains(a)) continue;
                StringBuilder b = new StringBuilder();
                for (Reference r : getReferencesTo(d.getAddress())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    b.append(' ').append(f == null ? "?" : f.getEntryPoint().toString()).append('@').append(r.getFromAddress());
                }
                println("STR " + d.getAddress() + " \"" + s + "\" <-" + b);
                break;
            }
        }
    }
}
