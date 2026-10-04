// Prints the address of every symbol whose name (with namespace) contains one of the given strings.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class Symbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext()) {
            Symbol s = it.next();
            String name = s.getName(true);
            for (String a : args) {
                if (name.contains(a)) {
                    println("SYM " + s.getAddress() + " " + name);
                    break;
                }
            }
        }
    }
}
