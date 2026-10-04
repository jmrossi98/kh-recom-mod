// Prints the instruction listing (address, bytes, text) for [start, end).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class Disasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Address end = toAddr(args[1]);
        Instruction ins = getInstructionContaining(toAddr(args[0]));
        while (ins != null && ins.getAddress().compareTo(end) < 0) {
            StringBuilder b = new StringBuilder();
            for (byte x : ins.getBytes()) b.append(String.format("%02X ", x));
            println(String.format("ASM %s  %-30s %s", ins.getAddress(), b, ins));
            ins = ins.getNext();
        }
    }
}
