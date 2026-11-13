// Exports function boundaries so the recompiler can work from capstone and Python
// instead of generating code inside Ghidra. Ghidra stays the analysis database; codegen
// lives where it is cheap to iterate.
//
// boot_depth is the distance from the entry point over direct calls, or -1 for functions
// outside that closure, which is how the first recompiler pass selects its subset.
//@category ALBW
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
import java.math.BigInteger;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.HashMap;
import java.util.Map;

public class AlbwExport extends GhidraScript {

    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0]
                : "ghidra/export/functions.tsv";

        Map<Long, Integer> bootDepth = new HashMap<>();
        Function entry = getFunctionAt(toAddr(0x00100000L));
        if (entry != null) {
            Deque<Function> q = new ArrayDeque<>();
            bootDepth.put(entry.getEntryPoint().getOffset(), 0);
            q.add(entry);
            while (!q.isEmpty()) {
                if (monitor.isCancelled()) return;
                Function f = q.poll();
                int d = bootDepth.get(f.getEntryPoint().getOffset());

                java.util.List<Function> next = new java.util.ArrayList<>(
                        f.getCalledFunctions(monitor));

                // Tail calls reach a function by branching, not calling, and a body can
                // also simply run off its end into the next function. A closure over
                // calls alone misses both, and the recompiler then sees targets it has
                // to emit but was never told about.
                var it2 = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (it2.hasNext()) {
                    var ins = it2.next();
                    var ft = ins.getFlowType();
                    if (ft.isJump() && !ft.isComputed()) {
                        for (var t : ins.getFlows()) {
                            if (f.getBody().contains(t)) continue;
                            Function tf = getFunctionAt(t);
                            if (tf != null) next.add(tf);
                        }
                    }
                    Address fall = ins.getFallThrough();
                    if (fall != null && !f.getBody().contains(fall)) {
                        Function ff = getFunctionAt(fall);
                        if (ff != null) next.add(ff);
                    }
                }

                for (Function c : next) {
                    long k = c.getEntryPoint().getOffset();
                    if (!bootDepth.containsKey(k)) {
                        bootDepth.put(k, d + 1);
                        q.add(c);
                    }
                }
            }
        }

        var tmode = currentProgram.getProgramContext().getRegister("TMode");
        java.io.File f = new java.io.File(out);
        f.getParentFile().mkdirs();
        PrintWriter w = new PrintWriter(f);
        // ranges lists every address range of the body as start-end pairs. Function
        // bodies are often non-contiguous, and reading addr..addr+size instead would
        // both miss the later fragments and swallow unrelated functions in between.
        w.println("addr\tend\tsize\tthumb\tboot_depth\tthunk\tranges\tname");

        int n = 0, thumb = 0, boot = 0;
        for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) break;
            Address a = fn.getEntryPoint();
            long start = a.getOffset();
            long size = fn.getBody().getNumAddresses();
            long end = fn.getBody().getMaxAddress().getOffset();

            int isThumb = 0;
            if (tmode != null) {
                BigInteger v = currentProgram.getProgramContext().getValue(tmode, a, false);
                if (v != null && v.intValue() == 1) isThumb = 1;
            }
            int d = bootDepth.getOrDefault(start, -1);

            StringBuilder ranges = new StringBuilder();
            for (var r : fn.getBody().getAddressRanges()) {
                if (ranges.length() > 0) ranges.append(';');
                ranges.append(String.format("%08X-%08X",
                        r.getMinAddress().getOffset(), r.getMaxAddress().getOffset()));
            }

            w.printf("%08X\t%08X\t%d\t%d\t%d\t%d\t%s\t%s%n",
                    start, end, size, isThumb, d, fn.isThunk() ? 1 : 0,
                    ranges, fn.getName());
            n++;
            if (isThumb == 1) thumb++;
            if (d >= 0) boot++;
        }
        w.close();
        println("export: wrote " + n + " functions (" + thumb + " thumb, "
                + boot + " in boot closure) to " + out);
    }
}
