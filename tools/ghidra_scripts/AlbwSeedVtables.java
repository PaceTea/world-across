// Seeds function starts from vtables and other .text pointers held in .rodata/.data.
//
// ALBW dispatches most calls virtually, so ~80% of computed calls have no statically
// resolvable target and the code they reach is never disassembled. The targets are
// still enumerable: they are stored as words in .rodata. Creating pointer data at the
// vtable slots gives Ghidra real references, and seeding the targets as functions
// breaks the chicken-and-egg between coverage and indirect-call resolution.
//@category ALBW
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class AlbwSeedVtables extends GhidraScript {

    private static final int MIN_RUN = 3;

    @Override
    public void run() throws Exception {
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        long textStart = text.getStart().getOffset();
        long textEnd = text.getEnd().getOffset();

        List<long[]> ptrs = new ArrayList<>();  // {sourceAddr, target}
        for (String name : new String[] { ".rodata", ".data" }) {
            MemoryBlock b = currentProgram.getMemory().getBlock(name);
            if (b == null) {
                println("seed: missing block " + name);
                continue;
            }
            for (long a = b.getStart().getOffset(); a + 3 <= b.getEnd().getOffset(); a += 4) {
                if (monitor.isCancelled()) return;
                Address src = toAddr(a);
                int w;
                try {
                    w = currentProgram.getMemory().getInt(src);
                } catch (Exception e) {
                    continue;
                }
                long t = w & 0xFFFFFFFFL;
                if (t >= textStart && t <= textEnd) {
                    ptrs.add(new long[] { a, t });
                }
            }
        }
        println("seed: " + ptrs.size() + " words point into .text");

        // Consecutive pointer runs are vtables; typing them yields real references.
        int runs = 0, typed = 0, typeFailed = 0;
        int i = 0;
        while (i < ptrs.size()) {
            int j = i;
            while (j + 1 < ptrs.size() && ptrs.get(j + 1)[0] == ptrs.get(j)[0] + 4) {
                j++;
            }
            int len = j - i + 1;
            if (len >= MIN_RUN) {
                runs++;
                for (int k = i; k <= j; k++) {
                    Address src = toAddr(ptrs.get(k)[0]);
                    try {
                        clearListing(src, src.add(3));
                        createData(src, new PointerDataType());
                        typed++;
                    } catch (Exception e) {
                        typeFailed++;
                    }
                }
            }
            i = j + 1;
        }
        println("seed: typed " + typed + " slots across " + runs
                + " vtable-like runs (" + typeFailed + " failed)");

        Set<Long> targets = new LinkedHashSet<>();
        for (long[] p : ptrs) {
            targets.add(p[1]);
        }

        int created = 0, already = 0, insideExisting = 0, unaligned = 0, failed = 0;
        for (long t : targets) {
            if (monitor.isCancelled()) return;
            if ((t & 3) != 0) {
                unaligned++;      // Thumb targets are odd; ALBW is ~all ARM, so skip.
                continue;
            }
            Address a = toAddr(t);
            if (getFunctionAt(a) != null) {
                already++;
                continue;
            }
            Function containing = getFunctionContaining(a);
            if (containing != null) {
                insideExisting++; // would split a body Ghidra already decided on
                continue;
            }
            try {
                disassemble(a);
                if (createFunction(a, null) != null) {
                    created++;
                } else {
                    failed++;
                }
            } catch (Exception e) {
                failed++;
            }
        }

        println("seed: distinct targets      : " + targets.size());
        println("seed:   functions created   : " + created);
        println("seed:   already a function  : " + already);
        println("seed:   inside existing body: " + insideExisting);
        println("seed:   unaligned (skipped) : " + unaligned);
        println("seed:   failed              : " + failed);
    }
}
