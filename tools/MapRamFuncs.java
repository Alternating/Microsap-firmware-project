// Map .data RAM image as executable block; create RAM functions + flash veneers.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import java.io.*;
import java.math.BigInteger;

public class MapRamFuncs extends GhidraScript {
    private Address addr(long a) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
    }
    private void thumbFunc(long a, String name) throws Exception {
        long t = a & ~1L;
        Address ad = addr(t);
        Register tmode = currentProgram.getRegister("TMode");
        if (currentProgram.getListing().getInstructionAt(ad) == null) {
            try { currentProgram.getProgramContext().setRegisterValue(ad, ad,
                new RegisterValue(tmode, BigInteger.ONE)); } catch (Exception e) {}
            new DisassembleCommand(ad, null, true).applyTo(currentProgram, monitor);
        }
        Function f = getFunctionAt(ad);
        if (f == null) f = createFunction(ad, name);
        if (f != null && name != null && f.getName().startsWith("FUN_"))
            f.setName(name, SourceType.USER_DEFINED);
    }
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        // 1. RAM .data block from flash image tail: flash 0x100a0198 -> ram 0x20000188, 12756 bytes
        if (mem.getBlock(addr(0x20000188L)) == null) {
            byte[] all = new FileInputStream(
                "/projects/Pico2maple-decomp/pico2maple_flipped-bt_rp2350-arm-s_0x10000000.bin").readAllBytes();
            byte[] seg = new byte[0x2000335c - 0x20000188];
            System.arraycopy(all, 0xa0198, seg, 0, seg.length);
            MemoryBlock b = mem.createInitializedBlock("ram_data", addr(0x20000188L),
                new ByteArrayInputStream(seg), seg.length, monitor, false);
            b.setExecute(true); b.setRead(true); b.setWrite(true);
            println("ram_data block created");
        }
        // 2. RAM functions (targets of flash veneers)
        long[] ramFns = {0x20000189L,0x200001fdL,0x20000261L,0x200002bdL,0x2000033dL,
            0x2000037fL,0x200003c9L,0x20000421L,0x20000491L,0x200004fdL,0x20000535L,
            0x20000571L,0x200005c1L,0x20000621L,0x200007b1L,0x2000085dL,0x20000939L,
            0x20000985L,0x200009d1L,0x20000b45L,0x20000c61L,0x20000d45L,0x20000dadL,
            0x20000e3dL,0x2000116dL,0x2000121dL,0x200012bdL};
        for (long a : ramFns) thumbFunc(a, String.format("ram_fn_%08x", a & ~1L));
        // 3. Flash veneers at 0x10056fa8..0x1005705c (ldr.w pc,[pc] + literal)
        for (long v = 0x10056fa8L; v <= 0x1005705cL; v += 8) {
            long tgt = mem.getInt(addr(v + 4)) & 0xffffffffL;
            thumbFunc(v | 1, String.format("veneer_to_%08x", tgt & ~1L));
        }
        // 4. extra pair at 0x10057060
        thumbFunc(0x1000aaa9L, null); thumbFunc(0x1000aa1dL, null);
        println("MAPRAM DONE, functions now: " + currentProgram.getFunctionManager().getFunctionCount());
    }
}
