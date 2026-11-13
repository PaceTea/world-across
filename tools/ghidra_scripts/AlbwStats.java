// Reports the numbers that scope the recompiler: function count, ARM/Thumb split,
// code coverage of .text, unresolved computed branches, and the SVC set actually issued.
//@category ALBW
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.FlowType;
import java.math.BigInteger;
import java.util.Map;
import java.util.TreeMap;

public class AlbwStats extends GhidraScript {

    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        Register tmode = currentProgram.getProgramContext().getRegister("TMode");

        int total = 0, thumbFns = 0, armFns = 0, thunks = 0, noReturn = 0;
        long funcBytes = 0;
        for (Function f : fm.getFunctions(true)) {
            total++;
            if (f.isThunk()) thunks++;
            if (f.hasNoReturn()) noReturn++;
            funcBytes += f.getBody().getNumAddresses();
            if (tmode != null) {
                BigInteger v = currentProgram.getProgramContext()
                        .getValue(tmode, f.getEntryPoint(), false);
                if (v != null && v.intValue() == 1) thumbFns++;
                else armFns++;
            }
        }

        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        AddressSet textSet = new AddressSet(text.getStart(), text.getEnd());
        long textSize = text.getSize();

        int insCount = 0, thumbIns = 0, computedJump = 0, unresolvedJump = 0,
                computedCall = 0, unresolvedCall = 0, svcCount = 0;
        long insBytes = 0;
        Map<Long, Integer> svcs = new TreeMap<>();

        InstructionIterator it = listing.getInstructions(textSet, true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            insCount++;
            insBytes += ins.getLength();
            if (ins.getLength() == 2) thumbIns++;

            FlowType ft = ins.getFlowType();
            if (ft.isComputed()) {
                boolean unresolved = ins.getFlows().length == 0;
                if (ft.isCall()) {
                    computedCall++;
                    if (unresolved) unresolvedCall++;
                } else if (ft.isJump()) {
                    computedJump++;
                    if (unresolved) unresolvedJump++;
                }
            }

            String m = ins.getMnemonicString().toLowerCase();
            if (m.startsWith("svc") || m.startsWith("swi")) {
                svcCount++;
                Scalar s = ins.getScalar(0);
                long imm = (s == null) ? -1 : s.getUnsignedValue();
                svcs.merge(imm, 1, Integer::sum);
            }
        }

        println("");
        println("=== functions ===");
        println(String.format("total              : %d", total));
        println(String.format("  arm / thumb      : %d / %d", armFns, thumbFns));
        println(String.format("  thunks           : %d", thunks));
        println(String.format("  noreturn         : %d", noReturn));
        println(String.format("bytes in functions : 0x%X of 0x%X .text (%.1f%%)",
                funcBytes, textSize, 100.0 * funcBytes / textSize));

        println("");
        println("=== .text coverage ===");
        println(String.format("instructions       : %d", insCount));
        println(String.format("  16-bit (thumb)   : %d (%.1f%%)",
                thumbIns, 100.0 * thumbIns / Math.max(insCount, 1)));
        println(String.format("disassembled bytes : 0x%X of 0x%X (%.1f%%)",
                insBytes, textSize, 100.0 * insBytes / textSize));
        println(String.format("undisassembled     : 0x%X", textSize - insBytes));

        println("");
        println("=== static-recomp risk ===");
        println(String.format("computed jumps     : %d  (unresolved: %d)",
                computedJump, unresolvedJump));
        println(String.format("computed calls     : %d  (unresolved: %d)",
                computedCall, unresolvedCall));

        println("");
        println("=== syscalls issued ===");
        println(String.format("svc instructions   : %d, distinct immediates: %d",
                svcCount, svcs.size()));
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<Long, Integer> e : svcs.entrySet()) {
            sb.append(String.format("0x%02X(x%d) ", e.getKey(), e.getValue()));
        }
        println("   " + sb.toString().trim());
    }
}
