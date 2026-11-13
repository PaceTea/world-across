// Maps the boot path: the direct-call closure from the entry point, which functions
// issue syscalls, and the entry disassembly. This is the subset the first recompiler
// pass has to translate, so its size decides whether a vertical slice is feasible.
//
// Only direct calls are followed. ALBW dispatches ~80% of calls virtually, but that
// starts once object graphs exist; early init is predominantly direct, so this closure
// is a good approximation of the boot path rather than of the whole program.
//@category ALBW
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

public class AlbwBootPath extends GhidraScript {

    private static final int ENTRY_DUMP = 80;

    @Override
    public void run() throws Exception {
        Address entryAddr = toAddr(0x00100000L);
        Function entry = getFunctionAt(entryAddr);
        if (entry == null) {
            println("boot: no function at entry");
            return;
        }

        Map<Function, Integer> depth = new LinkedHashMap<>();
        Deque<Function> queue = new ArrayDeque<>();
        depth.put(entry, 0);
        queue.add(entry);

        long bodyBytes = 0;
        while (!queue.isEmpty()) {
            if (monitor.isCancelled()) return;
            Function f = queue.poll();
            int d = depth.get(f);
            bodyBytes += f.getBody().getNumAddresses();
            for (Function callee : f.getCalledFunctions(monitor)) {
                if (!depth.containsKey(callee)) {
                    depth.put(callee, d + 1);
                    queue.add(callee);
                }
            }
        }

        println("");
        println("=== boot closure (direct calls from entry) ===");
        println("functions reachable : " + depth.size());
        println("bytes in bodies     : 0x" + Long.toHexString(bodyBytes)
                + " (" + (bodyBytes / 1024) + " KiB)");

        Map<Integer, Integer> perDepth = new TreeMap<>();
        for (int d : depth.values()) {
            perDepth.merge(d, 1, Integer::sum);
        }
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<Integer, Integer> e : perDepth.entrySet()) {
            sb.append("d").append(e.getKey()).append("=").append(e.getValue()).append(" ");
        }
        println("depth distribution  : " + sb.toString().trim());

        // Which of these issue syscalls, and which immediates.
        List<String> svcFns = new ArrayList<>();
        Map<Long, Integer> svcTotals = new TreeMap<>();
        for (Map.Entry<Function, Integer> e : depth.entrySet()) {
            Function f = e.getKey();
            Map<Long, Integer> local = new TreeMap<>();
            InstructionIterator it = currentProgram.getListing()
                    .getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                String m = ins.getMnemonicString().toLowerCase();
                if (m.startsWith("svc") || m.startsWith("swi")) {
                    Scalar s = ins.getScalar(0);
                    long imm = (s == null) ? -1 : s.getUnsignedValue();
                    local.merge(imm, 1, Integer::sum);
                    svcTotals.merge(imm, 1, Integer::sum);
                }
            }
            if (!local.isEmpty()) {
                StringBuilder l = new StringBuilder();
                for (Map.Entry<Long, Integer> x : local.entrySet()) {
                    l.append(String.format("0x%02X(x%d) ", x.getKey(), x.getValue()));
                }
                svcFns.add(String.format("  %-34s @%s d%d : %s",
                        f.getName(), f.getEntryPoint(), e.getValue(), l.toString().trim()));
            }
        }
        println("");
        println("=== syscall sites inside the closure ===");
        println("functions issuing svc : " + svcFns.size());
        StringBuilder tot = new StringBuilder();
        for (Map.Entry<Long, Integer> x : svcTotals.entrySet()) {
            tot.append(String.format("0x%02X(x%d) ", x.getKey(), x.getValue()));
        }
        println("immediates            : " + tot.toString().trim());
        for (String s : svcFns) {
            println(s);
        }

        println("");
        println("=== entry function ===");
        println("name/size : " + entry.getName() + "  0x"
                + Long.toHexString(entry.getBody().getNumAddresses()) + " bytes");
        println("direct callees : " + entry.getCalledFunctions(monitor).size());
        InstructionIterator it = currentProgram.getListing()
                .getInstructions(entry.getBody(), true);
        int n = 0;
        while (it.hasNext() && n++ < ENTRY_DUMP) {
            Instruction ins = it.next();
            println(String.format("  %s  %-10s %s",
                    ins.getAddress(), ins.getMnemonicString(), ins.toString()
                            .substring(Math.min(ins.getMnemonicString().length(),
                                    ins.toString().length()))));
        }
    }
}
