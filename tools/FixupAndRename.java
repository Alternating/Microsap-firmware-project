// Headless post-script: create functions from vector table + prologue scan in gaps, apply anchor names.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.mem.Memory;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import java.math.BigInteger;

public class FixupAndRename extends GhidraScript {
    private Address addr(long a) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
    }
    private void makeThumbFunc(long a, String name) throws Exception {
        long target = a & ~1L;
        if (target < 0x10000000L || target >= 0x10057000L) return;
        Address ad = addr(target);
        // set TMode context and disassemble if needed
        Register tmode = currentProgram.getRegister("TMode");
        if (currentProgram.getListing().getInstructionAt(ad) == null) {
            try {
                currentProgram.getProgramContext().setRegisterValue(ad, ad,
                    new RegisterValue(tmode, BigInteger.ONE));
            } catch (Exception e) {}
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
        // 1. Vector table at image base: entries 1..67 (reset + exceptions + 52 IRQs)
        int made = 0;
        for (int i = 1; i < 68; i++) {
            long v = mem.getInt(addr(0x10000000L + i*4L)) & 0xffffffffL;
            if (v >= 0x10000001L && v < 0x10057000L && (v & 1) == 1) {
                makeThumbFunc(v, null); made++;
            }
        }
        println("vector handlers processed: " + made);
        // 2. Prologue scan in gaps across app+lib code
        FunctionManager fm = currentProgram.getFunctionManager();
        long scanStart = 0x10000200L, scanEnd = 0x10057000L;
        int created = 0;
        for (long a = scanStart; a < scanEnd; a += 2) {
            if (fm.getFunctionContaining(addr(a)) != null) {
                Function f = fm.getFunctionContaining(addr(a));
                a = f.getBody().getMaxAddress().getOffset() & ~1L; // skip to func end
                continue;
            }
            int b0 = mem.getByte(addr(a)) & 0xff, b1 = mem.getByte(addr(a+1)) & 0xff;
            boolean prologue = (b1 == 0xb5) // push {...,lr}
                || (b0 == 0x2d && b1 == 0xe9); // stmdb sp!,{...} (.w push)
            if (prologue) {
                int before = fm.getFunctionCount();
                makeThumbFunc(a | 1, null);
                if (fm.getFunctionCount() > before) created++;
            }
        }
        println("gap functions created: " + created);
        // 3. Anchor renames
        String[][] names = {
            {"10000608","main"},
            {"1000066c","maple_build_device_info_blocks"},
            {"10000824","vmu_build_device_info"},
            {"10002568","vmu_maple_cmd_handler_a"},
            {"10002948","vmu_maple_cmd_handler_b"},
            {"100039d0","sd_ensure_dirs"},
            {"10003abc","vmu_bank_filename"},
            {"10003ae0","gameid_path_build"},
            {"10003bc8","vmu_bank_file_fn"},
            {"10003bf4","gameid_vmu_path_build"},
            {"10003e94","settings_fn_3e94"},
            {"10003f48","settings_fn_3f48"},
            {"10004120","settings_fn_4120"},
            {"10004218","settings_fn_4218"},
            {"1000498c","cfg_parse_mapping_file"},
            {"10004d60","cfg_write_sample_file"},
            {"100073f4","tuh_mount_cb"},
            {"10007400","tuh_umount_cb"},
            {"100082dc","p2m_bp32_init"},
            {"100082e0","p2m_bp32_on_device_discovered"},
            {"100082ec","p2m_bp32_on_device_disconnected"},
            {"100082fc","p2m_bp32_on_device_ready"},
            {"10008358","p2m_bp32_get_property"},
            {"1000835c","p2m_bp32_on_oob_event"},
            {"100083d4","p2m_bp32_on_device_connected"},
            {"100083e4","p2m_bp32_on_init_complete"},
            {"10008b68","p2m_bp32_on_controller_data"},
            {"1000904c","panic"},
        };
        for (String[] n : names) {
            long a = Long.parseLong(n[0], 16);
            makeThumbFunc(a | 1, n[1]);
            Function f = getFunctionAt(addr(a));
            if (f != null && !f.getName().equals(n[1])) f.setName(n[1], SourceType.USER_DEFINED);
        }
        // 4. Data labels
        createLabel(addr(0x1005e4acL), "maple_tx_pio_program", true);
        createLabel(addr(0x1005e4f0L), "maple_rx_pio_program", true);
        createLabel(addr(0x100a2a18L), "p2m_uni_platform_struct", true);
        println("FIXUP DONE, total functions now: " + fm.getFunctionCount());
    }
}
