// Lays out ALBW's flat-loaded code.bin as .text/.rodata/.data/.bss and seeds the
// entry point, so auto-analysis gets correct permissions for constant propagation.
//@category ALBW
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class AlbwSetup extends GhidraScript {

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Address text = toAddr(0x00100000L);
        Address ro = toAddr(0x00693000L);
        Address data = toAddr(0x006FF000L);
        Address bss = toAddr(0x00728000L);

        try {
            mem.split(mem.getBlock(text), ro);
            mem.split(mem.getBlock(ro), data);

            MemoryBlock t = mem.getBlock(text);
            t.setName(".text");
            t.setRead(true);
            t.setWrite(false);
            t.setExecute(true);

            MemoryBlock r = mem.getBlock(ro);
            r.setName(".rodata");
            r.setRead(true);
            r.setWrite(false);
            r.setExecute(false);

            MemoryBlock d = mem.getBlock(data);
            d.setName(".data");
            d.setRead(true);
            d.setWrite(true);
            d.setExecute(false);

            mem.createUninitializedBlock(".bss", bss, 0x87A44L, false);
            println("AlbwSetup: segments laid out");
        } catch (Exception e) {
            println("AlbwSetup: segment layout failed: " + e);
        }

        addEntryPoint(text);
        disassemble(text);
        createFunction(text, "_start");
        println("AlbwSetup: entry point seeded at 0x00100000");
    }
}
