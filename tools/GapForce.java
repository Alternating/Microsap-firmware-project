// Force function creation in the 0x100017f2-0x10002250 gap and decompile them.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import java.io.*;
import java.math.BigInteger;

public class GapForce extends GhidraScript {
    private Address addr(long a) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
    }
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();
        Register tmode = currentProgram.getRegister("TMode");
        long lo = 0x100017f2L, hi = 0x10002250L;
        // scan for prologues; skip literal pools by stepping 2 and testing patterns
        int created = 0;
        for (long a = lo; a < hi; a += 2) {
            Function have = fm.getFunctionAt(addr(a));
            if (have != null) continue;
            int b0 = mem.getByte(addr(a)) & 0xff, b1 = mem.getByte(addr(a + 1)) & 0xff;
            boolean prologue = (b1 == 0xb5) || (b0 == 0x2d && b1 == 0xe9);
            if (!prologue) continue;
            Function containing = fm.getFunctionContaining(addr(a));
            if (containing != null) continue;
            try {
                currentProgram.getProgramContext().setRegisterValue(addr(a), addr(a),
                    new RegisterValue(tmode, BigInteger.ONE));
            } catch (Exception e) {}
            if (currentProgram.getListing().getInstructionAt(addr(a)) == null)
                new DisassembleCommand(addr(a), null, true).applyTo(currentProgram, monitor);
            Function f = createFunction(addr(a), String.format("gapfn_%08x", a));
            if (f != null) {
                created++;
                println("created gapfn at " + Long.toHexString(a) + " size " + f.getBody().getNumAddresses());
                long end = f.getBody().getMaxAddress().getOffset();
                if (end > a && end < hi) a = end & ~1L;
            }
        }
        println("gap functions created: " + created);
        // decompile everything overlapping the gap + all gapfn/ram_fn into a side file
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        PrintWriter out = new PrintWriter(new FileWriter("/projects/Pico2maple-decomp/export/gap_functions.c"));
        for (Function f : fm.getFunctions(true)) {
            long e = f.getEntryPoint().getOffset();
            if ((e >= lo && e < hi) || f.getName().startsWith("gapfn_")) {
                DecompileResults r = decomp.decompileFunction(f, 60, monitor);
                out.println("// ==== " + f.getEntryPoint() + " " + f.getName() + " (size " +
                    f.getBody().getNumAddresses() + ") ====");
                if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null)
                    out.println(r.getDecompiledFunction().getC());
                else out.println("// FAILED");
            }
        }
        out.close();
        decomp.dispose();
        println("GAPFORCE DONE");
    }
}
