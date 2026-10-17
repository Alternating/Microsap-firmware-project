// Headless post-script: export decompiled C for all functions, plus CSV inventories.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import ghidra.program.model.address.Address;
import java.io.*;

public class DecompExportAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/tmp/export";
        new File(outDir).mkdirs();

        // 1. functions.csv
        PrintWriter fcsv = new PrintWriter(new FileWriter(outDir + "/functions.csv"));
        fcsv.println("address,name,size,num_callers,num_callees");
        FunctionManager fm = currentProgram.getFunctionManager();
        int total = fm.getFunctionCount();
        for (Function f : fm.getFunctions(true)) {
            int callers = f.getCallingFunctions(monitor).size();
            int callees = f.getCalledFunctions(monitor).size();
            fcsv.println(f.getEntryPoint() + "," + f.getName() + "," +
                f.getBody().getNumAddresses() + "," + callers + "," + callees);
        }
        fcsv.close();

        // 2. strings.csv with xref'ing functions
        PrintWriter scsv = new PrintWriter(new FileWriter(outDir + "/strings.csv"));
        scsv.println("address|string|referencing_functions");
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        ReferenceManager rm = currentProgram.getReferenceManager();
        while (di.hasNext() && !monitor.isCancelled()) {
            Data d = di.next();
            if (!(d.getDataType() instanceof StringDataType) &&
                !(d.getDataType() instanceof TerminatedStringDataType)) continue;
            Object v = d.getValue();
            if (v == null) continue;
            StringBuilder refs = new StringBuilder();
            ReferenceIterator ri = rm.getReferencesTo(d.getAddress());
            while (ri.hasNext()) {
                Reference r = ri.next();
                Function rf = fm.getFunctionContaining(r.getFromAddress());
                if (rf != null) { if (refs.length() > 0) refs.append(";"); refs.append(rf.getEntryPoint() + ":" + rf.getName()); }
            }
            String sv = v.toString().replace("\n", "\\n").replace("\r", "\\r").replace("|", "\\x7c");
            scsv.println(d.getAddress() + "|" + sv + "|" + refs);
        }
        scsv.close();

        // 3. decompile everything into one file
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        PrintWriter cout = new PrintWriter(new BufferedWriter(new FileWriter(outDir + "/decompiled_all.c"), 1 << 20));
        int i = 0, ok = 0, fail = 0;
        for (Function f : fm.getFunctions(true)) {
            if (monitor.isCancelled()) break;
            i++;
            if (i % 200 == 0) println("decompiled " + i + "/" + total);
            if (f.isThunk()) continue;
            DecompileResults res = decomp.decompileFunction(f, 45, monitor);
            cout.println("// ==== " + f.getEntryPoint() + " " + f.getName() + " (size " +
                f.getBody().getNumAddresses() + ") ====");
            if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                cout.println(res.getDecompiledFunction().getC());
                ok++;
            } else {
                cout.println("// DECOMPILATION FAILED: " + (res != null ? res.getErrorMessage() : "null"));
                fail++;
            }
        }
        cout.close();
        decomp.dispose();
        println("EXPORT DONE: " + ok + " ok, " + fail + " failed, dir=" + outDir);
    }
}
