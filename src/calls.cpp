// Shadow of War Trainer - fault handling for guarded game calls.
// FN.CALL {addr, args:[...], ret:"u64"|"f32"|"f64", main_thread:true, confirm:true}
//   args (up to 16): number | "0x.." / address expression | true/false | "str:text" (UTF-8 C string) | "wstr:text" |
//         {"f32":1.5} | {"f64":2.0} | {"buf":64} (zeroed buffer, returned after the call) | {"hex":"01 02 .."} (buffer with bytes)
//   Windows x64: the first four go in rcx/rdx/r8/r9 or xmm0-3 by position, the rest on the stack.
//   Buffers are read back after the call (out parameters) and returned as hex.
// MEM.ALLOC {size, exec:false} / MEM.FREE {addr}: memory owned by the bridge (structs, out params, code).
#include "common.h"
#include <map>

namespace wb {


// Where and what a guarded game call faulted on: instruction address + (access violations) read/write and the address.
int fault_filter(EXCEPTION_POINTERS* ep, FaultInfo* fi) {
    fi->code = ep->ExceptionRecord->ExceptionCode;
    fi->at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (fi->code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        fi->rw = (int)ep->ExceptionRecord->ExceptionInformation[0];          // 0 read, 1 write, 8 execute
        fi->addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
std::string fault_text(const FaultInfo& f) {
    std::string where = module_name_of(f.at).empty() ? hexstr(f.at)
        : ((_stricmp(module_name_of(f.at).c_str(), kGameExe) == 0 ? std::string("game") : module_name_of(f.at)) + "+" + hexstr(f.at - module_base(module_name_of(f.at))));
    std::string s = "game function faulted with " + hexstr(f.code) + " at " + where;
    if (f.code == EXCEPTION_ACCESS_VIOLATION) s += std::string(f.rw == 1 ? " (write " : f.rw == 8 ? " (execute " : " (read ") + hexstr(f.addr) + ")";
    return s;
}

} // namespace wb
