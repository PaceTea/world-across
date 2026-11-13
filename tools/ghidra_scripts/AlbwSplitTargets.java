// Creates functions at branch targets that no function starts at.
//
// ALBW has many small thunks that load arguments and tail-call a shared body with `b`.
// Ghidra attributes such a shared body to whichever function reached it first, as a
// second fragment, leaving the other callers branching into the middle of a function.
// A recompiler emitting one C function per guest function cannot express that: a goto
// cannot cross function boundaries. Giving each such target its own entry makes every
// tail call expressible as a call.
//
// Works as a worklist from the boot closure outward, because splitting one function
// exposes targets in the functions it creates.
//@category ALBW
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.FlowType;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.HashSet;
import java.util.Set;

public class AlbwSplitTargets extends GhidraScript {

    @Override
    public void run() throws Exception {
        Deque<Function> work = new ArrayDeque<>();
        Set<Long> seen = new HashSet<>();

        // Every function, not a closure from the entry point: the constructors reached
        // through the static initialiser table are never called directly, so a closure
        // over calls would skip exactly the functions that need splitting most.
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (seen.add(f.getEntryPoint().getOffset())) work.add(f);
        }
        println("split: starting from " + work.size() + " functions");

        int created = 0, failed = 0, examined = 0;
        while (!work.isEmpty()) {
            if (monitor.isCancelled()) break;
            Function f = work.poll();
            examined++;

            InstructionIterator it = currentProgram.getListing()
                    .getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();

                // A body can leave itself two ways that a recompiler cannot express as
                // anything but a call: branching out of it, or simply running off the end
                // of one of its ranges into the next address.
                java.util.List<Address> exits = new java.util.ArrayList<>();
                FlowType ft = ins.getFlowType();
                if (ft.isJump() && !ft.isComputed()) {
                    for (Address t : ins.getFlows()) exits.add(t);
                }
                Address fall = ins.getFallThrough();
                if (fall != null) exits.add(fall);

                for (Address t : exits) {
                    if (f.getBody().contains(t)) continue;      // ordinary local flow
                    if (getFunctionAt(t) != null) continue;     // already a function

                    try {
                        Function nf = createFunction(t, null);
                        if (nf != null) {
                            created++;
                            if (seen.add(t.getOffset())) work.add(nf);
                        } else {
                            failed++;
                        }
                    } catch (Exception e) {
                        failed++;
                    }
                }
            }
        }

        println("split: examined " + examined + " functions");
        println("split: created " + created + " new function entries (" + failed + " failed)");
    }
}
