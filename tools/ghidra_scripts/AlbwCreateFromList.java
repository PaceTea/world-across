// Creates functions at addresses listed in a file, one hex address per line.
//
// Used for targets that no scan of the binary can find on its own, such as the C++
// constructors reached through the static initialiser table, whose slots hold
// pc-relative offsets rather than pointers. Bit 0 set means a Thumb entry, so TMode is
// set before disassembling.
//@category ALBW
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import ghidra.program.model.listing.Function;
import java.io.BufferedReader;
import java.io.FileReader;
import java.math.BigInteger;
import java.util.LinkedHashSet;
import java.util.Set;

public class AlbwCreateFromList extends GhidraScript {

    @Override
    public void run() throws Exception {
        String path = getScriptArgs().length > 0 ? getScriptArgs()[0]
                : "ghidra/export/init_targets.txt";

        Set<Long> want = new LinkedHashSet<>();
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (!line.isEmpty()) want.add(Long.parseLong(line, 16));
            }
        }
        println("create: read " + want.size() + " addresses from " + path);

        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        int created = 0, already = 0, inside = 0, failed = 0;

        for (long raw : want) {
            if (monitor.isCancelled()) break;
            boolean isThumb = (raw & 1) == 1;
            Address a = toAddr(raw & ~1L);

            if (getFunctionAt(a) != null) {
                already++;
                continue;
            }
            if (getFunctionContaining(a) != null) {
                inside++;
                continue;
            }
            try {
                if (tmode != null) {
                    currentProgram.getProgramContext().setRegisterValue(a, a,
                            new RegisterValue(tmode,
                                    isThumb ? BigInteger.ONE : BigInteger.ZERO));
                }
                disassemble(a);
                Function f = createFunction(a, null);
                if (f != null) created++;
                else failed++;
            } catch (Exception e) {
                failed++;
            }
        }

        println("create:   functions created   : " + created);
        println("create:   already a function  : " + already);
        println("create:   inside existing body: " + inside);
        println("create:   failed              : " + failed);
    }
}
