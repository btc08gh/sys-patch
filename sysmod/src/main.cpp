#include <cstring>
#include <span>
#include <algorithm> // for std::min
#include <bit> // for std::byteswap
#include <utility> // std::unreachable
#include <switch.h>
#include "minIni/minIni.h"

namespace {

constexpr u64 INNER_HEAP_SIZE = 0x1000; // Size of the inner heap (adjust as necessary).
constexpr u64 READ_BUFFER_SIZE = 0x1000; // size of static buffer which memory is read into
constexpr u32 FW_VER_ANY = 0x0;
constexpr u16 REGEX_SKIP = 0x100;

u32 FW_VERSION{}; // set on startup
u32 AMS_VERSION{}; // set on startup
u32 AMS_TARGET_VERSION{}; // set on startup
u8 AMS_KEYGEN{}; // set on startup
u64 AMS_HASH{}; // set on startup
bool VERSION_SKIP{}; // set on startup

struct DebugEventInfo {
    u32 event_type;
    u32 flags;
    u64 thread_id;
    u64 title_id;
    u64 process_id;
    char process_name[12];
    u32 mmu_flags;
    u8 _0x30[0x10];
};

struct PatternData {
    constexpr PatternData(const char* s) {
        // skip leading 0x (if any)
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }

        // invalid string will cause a compile-time error due to no return
        constexpr auto hexstr_2_nibble = [](char c) -> u8 {
            if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
            if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
            if (c >= '0' && c <= '9') { return c - '0'; }
        };

        // parse and convert string
        while (*s != '\0') {
            if (*s == '.') {
                data[size] = REGEX_SKIP;
                s++;
            } else {
                data[size] |= hexstr_2_nibble(*s++) << 4;
                data[size] |= hexstr_2_nibble(*s++) << 0;
            }
            size++;
        }
    }

    // 32 is a reasonable max length for a byte pattern
    // will compile-time error is size is too small
    u16 data[32]{};
    u8 size{};
};

struct PatchData {
    template<typename T>
    constexpr PatchData(T _data) {
        data = _data;
        size = sizeof(T);
    }
    u64 data;
    u8 size;
};

enum class PatchedResult {
    NOT_FOUND,
    SKIPPED,
    PATCHED_FILE,
    PATCHED_SYSPATCH,
    FAILED_WRITE,
};

struct Patterns {
    const char* patch_name; // name of patch
    const PatternData byte_pattern; // the pattern to search

    const s32 inst_offset; // instruction offset relative to byte pattern
    const s32 patch_offset; // patch offset relative to inst_offset

    bool (*const cond)(u32 inst); // check condition of the instruction
    PatchData (*const patch)(u32 inst); // the patch data to be applied
    bool (*const applied)(u32 inst); // check to see if patch already applied

    const u32 min_fw_ver{FW_VER_ANY}; // set to FW_VER_ANY to ignore
    const u32 max_fw_ver{FW_VER_ANY}; // set to FW_VER_ANY to ignore
    const u32 min_ams_ver{FW_VER_ANY}; // set to FW_VER_ANY to ignore
    const u32 max_ams_ver{FW_VER_ANY}; // set to FW_VER_ANY to ignore

    PatchedResult result{PatchedResult::NOT_FOUND};
};

struct PatchEntry {
    const char* name; // name of the system title
    const u64 title_id; // title id of the system title
    const std::span<Patterns> patterns; // list of patterns to find
    const u32 min_fw_ver{FW_VER_ANY}; // set to FW_VER_ANY to ignore
    const u32 max_fw_ver{FW_VER_ANY}; // set to FW_VER_ANY to ignore
};

constexpr auto subi_cond(u32 inst) -> bool {
    // # Used on Atmosphère-NX 0.11.0 - 0.12.0.
    const auto type = (inst >> 24) & 0xFF;
    const auto imm = (inst >> 10) & 0xFFF;
    return (type == 0x71) && (imm == 0x0A);
}

constexpr auto subr_cond(u32 inst) -> bool {
    // # Used on Atmosphère-NX 0.13.0 and later.
    const auto type = (inst >> 21) & 0x7F9;
    const auto reg = (inst >> 16) & 0x1F;
    return (type == 0x358) && (reg == 0x01);
}

constexpr auto bl_cond(u32 inst) -> bool {
    return ((inst >> 26) & 0x3F) == 0x25;
}

constexpr auto tbz_cond(u32 inst) -> bool {
    return ((inst >> 24) & 0x7F) == 0x36;
}

constexpr auto subs_cond(u32 inst) -> bool {
    return subi_cond(inst) || subr_cond(inst);
}

constexpr auto cbz_cond(u32 inst) -> bool {
    const auto type = inst >> 24;
    return type == 0x34 || type == 0xB4;
}

constexpr auto mov_cond(u32 inst) -> bool {
    return ((inst >> 24) & 0x7F) == 0x52;
}

constexpr auto mov2_cond(u32 inst) -> bool {
    if (hosversionBefore(15,0,0)) {
        return (inst >> 24) == 0x92; // and x0, x19, #0xffffffff
    } else {
        return (inst >> 24) == 0x2A;
    }
}

constexpr auto bne_cond(u32 inst) -> bool {
    const auto type = inst >> 24;
    const auto cond = inst & 0x10;
    return type == 0x54 || cond == 0x0;
}

constexpr auto ret0_patch(u32 inst) -> PatchData {
    return std::byteswap(0xE0031F2AU);
}

constexpr auto nop_patch(u32 inst) -> PatchData {
    return std::byteswap(0x1F2003D5U);
}

constexpr auto subs_patch(u32 inst) -> PatchData {
    return subi_cond(inst) ? (u8)0x1 : (u8)0x0;
}

constexpr auto b_patch(u32 inst) -> PatchData {
    const auto opcode = 0x14 << 24;
    const auto offset = (inst >> 5) & 0x7FFFF;
    return opcode | offset;
}

constexpr auto mov0_patch(u32 inst) -> PatchData {
    return std::byteswap(0xE0031FAAU);
}

constexpr auto ret0_applied(u32 inst) -> bool {
    return ret0_patch(inst).data == inst;
}

constexpr auto nop_applied(u32 inst) -> bool {
    return nop_patch(inst).data == inst;
}

constexpr auto subs_applied(u32 inst) -> bool {
    const auto type_i = (inst >> 24) & 0xFF;
    const auto imm = (inst >> 10) & 0xFFF;
    const auto type_r = (inst >> 21) & 0x7F9;
    const auto reg = (inst >> 16) & 0x1F;
    return ((type_i == 0x71) && (imm == 0x1)) || ((type_r == 0x358) && (reg == 0x0));
}

constexpr auto b_applied(u32 inst) -> bool {
    return 0x14 == (inst >> 24);
}

constexpr auto mov0_applied(u32 inst) -> bool {
    return mov0_patch(inst).data == inst;
}

constinit Patterns fs_patterns[] = {
    { "noacidsigchk1", "0xC8FE4739", -24, 0, bl_cond, ret0_patch, ret0_applied, FW_VER_ANY, MAKEHOSVERSION(9,2,0) },
    { "noacidsigchk2", "0x0210911F000072", -5, 0, bl_cond, ret0_patch, ret0_applied, FW_VER_ANY, MAKEHOSVERSION(9,2,0) },
    { "noncasigchk_old", "0x1E42B9", -5, 0, tbz_cond, nop_patch, nop_applied, MAKEHOSVERSION(10,0,0), MAKEHOSVERSION(14,2,1) },
    { "noncasigchk_new", "0x3E4479", -5, 0, tbz_cond, nop_patch, nop_applied, MAKEHOSVERSION(15,0,0) },
    { "nocntchk_old", "0x081C00121F05007181000054", -4, 0, bl_cond, ret0_patch, ret0_applied, MAKEHOSVERSION(10,0,0), MAKEHOSVERSION(14,2,1) },
    { "nocntchk_new", "0x081C00121F05007141010054", -4, 0, bl_cond, ret0_patch, ret0_applied, MAKEHOSVERSION(15,0,0) },
};

constinit Patterns ldr_patterns[] = {
    { "noacidsigchk", "0xFD7BC6A8C0035FD6", 16, 2, subs_cond, subs_patch, subs_applied },
};

constinit Patterns es_patterns[] = {
    { "es1", "0x1F90013128928052", -4, 0, cbz_cond, b_patch, b_applied, FW_VER_ANY, MAKEHOSVERSION(13,2,1) },
    { "es2", "0xC07240F9E1930091", -4, 0, tbz_cond, nop_patch, nop_applied, FW_VER_ANY, MAKEHOSVERSION(10,2,0) },
    { "es3", "0xF3031FAA02000014", -4, 0, bne_cond, nop_patch, nop_applied, FW_VER_ANY, MAKEHOSVERSION(10,2,0) },
    { "es4", "0xC0FDFF35A8C35838", -4, 0, mov_cond, nop_patch, nop_applied, MAKEHOSVERSION(11,0,0), MAKEHOSVERSION(13,2,1) },
    { "es5", "0xE023009145EEFF97", -4, 0, cbz_cond, b_patch, b_applied, MAKEHOSVERSION(11,0,0), MAKEHOSVERSION(13,2,1) },
    { "es6", "0x.6300...0094A0..D1..FF97", 16, 0, mov2_cond, mov0_patch, mov0_applied, MAKEHOSVERSION(14,0,0) },
};

// NOTE: add system titles that you want to be patched to this table.
// a list of system titles can be found here https://switchbrew.org/wiki/Title_list
constinit PatchEntry patches[] = {
    { "fs", 0x0100000000000000, fs_patterns },
    // ldr needs to be patched in fw 10+
    { "ldr", 0x0100000000000001, ldr_patterns, MAKEHOSVERSION(10,0,0) },
    // es was added in fw 2
    { "es", 0x0100000000000033, es_patterns, MAKEHOSVERSION(2,0,0) },
};

struct EmummcPaths {
    char unk[0x80];
    char nintendo[0x80];
};

void smcAmsGetEmunandConfig(EmummcPaths* out_paths) {
    SecmonArgs args{};
    args.X[0] = 0xF0000404; /* smcAmsGetEmunandConfig */
    args.X[1] = 0; /* EXO_EMUMMC_MMC_NAND*/
    args.X[2] = (u64)out_paths; /* out path */
    svcCallSecureMonitor(&args);
}

auto is_emummc() -> bool {
    EmummcPaths paths{};
    smcAmsGetEmunandConfig(&paths);
    return (paths.unk[0] != '\0') || (paths.nintendo[0] != '\0');
}

void patcher(Handle handle, std::span<const u8> data, u64 addr, std::span<Patterns> patterns) {
    for (auto& p : patterns) {
        // skip if version isn't valid
        if (VERSION_SKIP &&
            ((p.min_fw_ver && p.min_fw_ver > FW_VERSION) ||
            (p.max_fw_ver && p.max_fw_ver < FW_VERSION) ||
            (p.min_ams_ver && p.min_ams_ver > AMS_VERSION) ||
            (p.max_ams_ver && p.max_ams_ver < AMS_VERSION))) {
            p.result = PatchedResult::SKIPPED;
            continue;
        }

        // skip if already patched
        if (p.result == PatchedResult::PATCHED_FILE || p.result == PatchedResult::PATCHED_SYSPATCH) {
            continue;
        }

        for (u32 i = 0; i < data.size(); i++) {
            if (i + p.byte_pattern.size >= data.size()) {
                break;
            }

            // loop through every byte of the pattern data to find a match
            // skipping over any bytes if the value is REGEX_SKIP
            u32 count{};
            while (count < p.byte_pattern.size) {
                if (p.byte_pattern.data[count] != data[i + count] && p.byte_pattern.data[count] != REGEX_SKIP) {
                    break;
                }
                count++;
            }

            // if we have found a matching pattern
            if (count == p.byte_pattern.size) {
                // fetch the instruction
                u32 inst{};
                const auto inst_offset = i + p.inst_offset;
                std::memcpy(&inst, data.data() + inst_offset, sizeof(inst));

                // check if the instruction is the one that we want
                if (p.cond(inst)) {
                    const auto [patch_data, patch_size] = p.patch(inst);
                    const auto patch_offset = addr + inst_offset + p.patch_offset;

                    // todo: log failed writes, although this should in theory never fail
                    if (R_FAILED(svcWriteDebugProcessMemory(handle, &patch_data, patch_offset, patch_size))) {
                        p.result = PatchedResult::FAILED_WRITE;
                    } else {
                        p.result = PatchedResult::PATCHED_SYSPATCH;
                    }
                    // move onto next pattern
                    break;
                } else if (p.applied(inst)) {
                    // patch already applied by sigpatches
                    p.result = PatchedResult::PATCHED_FILE;
                    break;
                }
            }
        }
    }
}

auto apply_patch(PatchEntry& patch) -> bool {
    Handle handle{};
    DebugEventInfo event_info{};

    u64 pids[0x50]{};
    s32 process_count{};
    static u8 buffer[READ_BUFFER_SIZE];

    // skip if version isn't valid
    if (VERSION_SKIP &&
        ((patch.min_fw_ver && patch.min_fw_ver > FW_VERSION) ||
        (patch.max_fw_ver && patch.max_fw_ver < FW_VERSION))) {
        for (auto& p : patch.patterns) {
            p.result = PatchedResult::SKIPPED;
        }
        return true;
    }

    if (R_FAILED(svcGetProcessList(&process_count, pids, 0x50))) {
        return false;
    }

    for (s32 i = 0; i < (process_count - 1); i++) {
        if (R_SUCCEEDED(svcDebugActiveProcess(&handle, pids[i])) &&
            R_SUCCEEDED(svcGetDebugEvent(&event_info, handle)) &&
            patch.title_id == event_info.title_id) {
            MemoryInfo mem_info{};
            u64 addr{};
            u32 page_info{};

            for (;;) {
                if (R_FAILED(svcQueryDebugProcessMemory(&mem_info, &page_info, handle, addr))) {
                    break;
                }
                addr = mem_info.addr + mem_info.size;

                // if addr=0 then we hit the reserved memory section
                if (!addr) {
                    break;
                }
                // skip memory that we don't want
                if (!mem_info.size || (mem_info.perm & Perm_Rx) != Perm_Rx || ((mem_info.type & 0xFF) != MemType_CodeStatic)) {
                    continue;
                }

                // todo: the byte pattern can in between 2 READ_BUFFER_SIZE boundries!
                for (u64 sz = 0; sz < mem_info.size; sz += READ_BUFFER_SIZE) {
                    const auto actual_size = std::min(READ_BUFFER_SIZE, mem_info.size);
                    if (R_FAILED(svcReadDebugProcessMemory(buffer, handle, mem_info.addr + sz, actual_size))) {
                        // todo: log failed reads!
                        break;
                    } else {
                        patcher(handle, std::span{buffer, actual_size}, mem_info.addr + sz, patch.patterns);
                    }
                }
            }
            svcCloseHandle(handle);
            return true;
        } else if (handle) {
            svcCloseHandle(handle);
            handle = 0;
        }
    }

    return false;
}

// creates a directory, non-recursive!
auto create_dir(const char* path) -> bool {
    Result rc{};
    FsFileSystem fs{};
    char path_buf[FS_MAX_PATH]{};

    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) {
        return false;
    }

    strcpy(path_buf, path);
    rc = fsFsCreateDirectory(&fs, path_buf);
    fsFsClose(&fs);
    return R_SUCCEEDED(rc);
}

// same as ini_get but writes out the default value instead
auto ini_load_or_write_default(const char* section, const char* key, long _default, const char* path) -> long {
    if (!ini_haskey(section, key, path)) {
        ini_putl(section, key, _default, path);
        return _default;
    } else {
        return ini_getl(section, key, _default, path);
    }
}

auto patch_result_to_str(PatchedResult result) -> const char* {
    switch (result) {
        case PatchedResult::NOT_FOUND: return "Unpatched";
        case PatchedResult::SKIPPED: return "Skipped";
        case PatchedResult::PATCHED_FILE: return "Patched (file)";
        case PatchedResult::PATCHED_SYSPATCH: return "Patched (sys-patch)";
        case PatchedResult::FAILED_WRITE: return "Failed (svcWriteDebugProcessMemory)";
    }

    std::unreachable();
}

void num_2_str(char*& s, u16 num) {
    u16 max_v = 1000;
    if (num > 9) {
        while (max_v >= 10) {
            if (num >= max_v) {
                while (max_v != 1) {
                    *s++ = '0' + (num / max_v);
                    num -= (num / max_v) * max_v;
                    max_v /= 10;
                }
            } else {
                max_v /= 10;
            }
        }
    }
    *s++ = '0' + (num); // always add 0 or 1's
}

void ms_2_str(char* s, u32 num) {
    u32 max_v = 100;
    *s++ = '0' + (num / 1000); // add seconds
    num -= (num / 1000) * 1000;
    *s++ = '.';

    while (max_v >= 10) {
        if (num >= max_v) {
            while (max_v != 1) {
                *s++ = '0' + (num / max_v);
                num -= (num / max_v) * max_v;
                max_v /= 10;
            }
        }
        else {
           *s++ = '0'; // append 0
           max_v /= 10;
        }
    }
    *s++ = '0' + (num); // always add 0 or 1's
    *s++ = 's'; // in seconds
}

// eg, 852481 -> 13.2.1
void version_to_str(char* s, u32 ver) {
    for (int i = 0; i < 3; i++) {
        num_2_str(s, (ver >> 16) & 0xFF);
        if (i != 2) {
            *s++ = '.';
        }
        ver <<= 8;
    }
}

// eg, 0xAF66FF99 -> AF66FF99
void hash_to_str(char* s, u32 hash) {
    for (int i = 0; i < 4; i++) {
        const auto num = (hash >> 24) & 0xFF;
        const auto top = (num >> 4) & 0xF;
        const auto bottom = (num >> 0) & 0xF;

        constexpr auto a = [](u8 nib) -> char {
            if (nib >= 0 && nib <= 9) { return '0' + nib; }
            return 'a' + nib - 10;
        };

        *s++ = a(top);
        *s++ = a(bottom);

        hash <<= 8;
    }
}

void keygen_to_str(char* s, u8 keygen) {
    num_2_str(s, keygen);
}

} // namespace

int main(int argc, char* argv[]) {
    constexpr auto ini_path = "/config/sys-patch/config.ini";
    constexpr auto log_path = "/config/sys-patch/log.ini";

    create_dir("/config/");
    create_dir("/config/sys-patch/");
    ini_remove(log_path);

    const auto patch_sysmmc = ini_load_or_write_default("options", "patch_sysmmc", 1, ini_path);
    const auto patch_emummc = ini_load_or_write_default("options", "patch_emummc", 1, ini_path);
    const auto enable_logging = ini_load_or_write_default("options", "enable_logging", 1, ini_path);
    VERSION_SKIP = ini_load_or_write_default("options", "version_skip", 1, ini_path);
    const auto emummc = is_emummc();
    bool enable_patching = true;

    // check if we should patch sysmmc
    if (!patch_sysmmc && !emummc) {
        enable_patching = false;
    }

    // check if we should patch emummc
    if (!patch_emummc && emummc) {
        enable_patching = false;
    }

    // speedtest
    const auto ticks_start = armGetSystemTick();

    if (enable_patching) {
        for (auto& patch : patches) {
            apply_patch(patch);
        }
    }

    const auto ticks_end = armGetSystemTick();
    const auto diff_ns = armTicksToNs(ticks_end) - armTicksToNs(ticks_start);

    if (enable_logging) {
        for (auto& patch : patches) {
            for (auto& p : patch.patterns) {
                if (!enable_patching) {
                    p.result = PatchedResult::SKIPPED;
                }
                ini_puts(patch.name, p.patch_name, patch_result_to_str(p.result), log_path);
            }
        }

        // fw of the system
        char fw_version[12]{};
        // atmosphere version
        char ams_version[12]{};
        // lowest fw supported by atmosphere
        char ams_target_version[12]{};
        // ???
        char ams_keygen[3]{};
        // git commit hash
        char ams_hash[9]{};
        // how long it took to patch
        char patch_time[20]{};

        version_to_str(fw_version, FW_VERSION);
        version_to_str(ams_version, AMS_VERSION);
        version_to_str(ams_target_version, AMS_TARGET_VERSION);
        keygen_to_str(ams_keygen, AMS_KEYGEN);
        hash_to_str(ams_hash, AMS_HASH >> 32);
        ms_2_str(patch_time, diff_ns/1000ULL/1000ULL);

        // defined in the Makefile
        #define DATE (DATE_DAY "." DATE_MONTH "." DATE_YEAR " " DATE_HOUR ":" DATE_MIN ":" DATE_SEC)

        ini_puts("stats", "version", VERSION_WITH_HASH, log_path);
        ini_puts("stats", "build_date", DATE, log_path);
        ini_puts("stats", "fw_version", fw_version, log_path);
        ini_puts("stats", "ams_version", ams_version, log_path);
        ini_puts("stats", "ams_target_version", ams_target_version, log_path);
        ini_puts("stats", "ams_keygen", ams_keygen, log_path);
        ini_puts("stats", "ams_hash", ams_hash, log_path);
        ini_putl("stats", "is_emummc", emummc, log_path);
        ini_putl("stats", "heap_size", INNER_HEAP_SIZE, log_path);
        ini_putl("stats", "buffer_size", READ_BUFFER_SIZE, log_path);
        ini_puts("stats", "patch_time", patch_time, log_path);
    }

    // note: sysmod exits here.
    // to keep it running, add a for (;;) loop (remember to sleep!)
    return 0;
}

// libnx stuff goes below
extern "C" {

// Sysmodules should not use applet*.
u32 __nx_applet_type = AppletType_None;

// Sysmodules will normally only want to use one FS session.
u32 __nx_fs_num_sessions = 1;

// Newlib heap configuration function (makes malloc/free work).
void __libnx_initheap(void) {
    static char inner_heap[INNER_HEAP_SIZE];
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    // Configure the newlib heap.
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

// Service initialization.
void __appInit(void) {
    Result rc{};

    // Open a service manager session.
    if (R_FAILED(rc = smInitialize()))
        fatalThrow(rc);

    // Retrieve the current version of Horizon OS.
    if (R_SUCCEEDED(rc = setsysInitialize())) {
        SetSysFirmwareVersion fw{};
        if (R_SUCCEEDED(rc = setsysGetFirmwareVersion(&fw))) {
            FW_VERSION = MAKEHOSVERSION(fw.major, fw.minor, fw.micro);
            hosversionSet(FW_VERSION);
        }
        setsysExit();
    }

    // get ams version
    if (R_SUCCEEDED(rc = splInitialize())) {
        u64 v{};
        u64 hash{};
        if (R_SUCCEEDED(rc = splGetConfig((SplConfigItem)65000, &v))) {
            AMS_VERSION = (v >> 40) & 0xFFFFFF;
            AMS_KEYGEN = (v >> 32) & 0xFF;
            AMS_TARGET_VERSION = v & 0xFFFFFF;
        }
        if (R_SUCCEEDED(rc = splGetConfig((SplConfigItem)65003, &hash))) {
            AMS_HASH = hash;
        }

        splExit();
    }

    if (R_FAILED(rc = fsInitialize()))
        fatalThrow(rc);

    // Add other services you want to use here.
    if (R_FAILED(rc = pmdmntInitialize()))
        fatalThrow(rc);

    // Close the service manager session.
    smExit();
}

// Service deinitialization.
void __appExit(void) {
    pmdmntExit();
    fsExit();
}

} // extern "C"
