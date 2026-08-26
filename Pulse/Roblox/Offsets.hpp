#pragma once

#include <cstdint>
#include <utility>
#include <Windows.h>

struct lua_State;
struct YieldState;
struct YieldingLuaThread;

#define REBASE(Address) (Address + reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)))

// Roblox client version this offset set was dumped against:
// ddf602d9cfe44005 (verification date not provided by the source).
// Not necessarily the client version actually running when this loads —
// per explicit instruction, proceeding anyway; a mismatch here would
// surface as a crash or corrupted reads at runtime, not a build error.
namespace Offsets
{
    const uintptr_t Print = REBASE(0x92C340);
    const uintptr_t OpcodeLookupTable = REBASE(0x6B83740);
    const uintptr_t ScriptContextResume = REBASE(0x22BBA10);
    const uintptr_t GetLuaStateForInstance = REBASE(0x2219D10);

    namespace Luau
    {
        const uintptr_t Luau_Execute = REBASE(0xB59570);
        const uintptr_t LuaO_NilObject = REBASE(0x610EFF8);
        const uintptr_t LuaH_DummyNode = REBASE(0x610EEB8);
    }

    namespace DataModel
    {
        const uintptr_t Children = 0x78;
        const uintptr_t GameLoaded = 0x570; // checked == 31 to confirm game loaded
        const uintptr_t ScriptContext = 0x440;
        const uintptr_t FakeDataModelToDataModel = 0x1D8;

        const uintptr_t FakeDataModelPointer = REBASE(0x8B79B58);
    }

    namespace ExtraSpace
    {
        const uintptr_t RequireBypass = 0x898;
        const uintptr_t ScriptContextToResume = 0x7E0;
    }
}

// Pulled in down here (not at the top) because PatternTable.hpp's own
// Table[] needs every Offsets::* constant above already defined to use as
// Fallback values. #pragma once makes this safe: PatternTable.hpp's own
// "#include <Roblox/Offsets.hpp>" resolves to a no-op re-entry (this file
// is already mid-include the first time this line runs), so it sees
// exactly the Offsets:: namespace content defined above this line.
#include <Roblox/PatternTable.hpp>

namespace Roblox
{
    // Resolved through Roblox::Patterns' signature scanner where a pattern
    // is filled in (see PatternTable.hpp), falling back to the REBASE()'d
    // constants above otherwise -- each Patterns::*() call caches its
    // result on first use. Tier 1.1 (signature scanning replacing
    // hardcoded offsets) is wired in here; see PatternTable.hpp for which
    // entries currently have a real pattern vs. still fall through.
    //
    // ROOT CAUSE of the 1114 (ERROR_DLL_INIT_FAILED) injection failure:
    // these four used to be `inline auto X = (FnPtr)Patterns::X();` --
    // namespace-scope `inline` variables with a non-constant initializer
    // still have static storage duration and are dynamically initialized
    // like any other global, which for a DLL means "runs during
    // DLL_PROCESS_ATTACH's global-constructor phase, before DllMain's own
    // body executes." That ran a full AOB byte-scan over the host
    // module's image (Patterns::Resolve -> Scanner::FindPatternInModule)
    // as part of static init -- unguarded by anything (Debug::Guard is
    // only ever called from inside MainThread, which hadn't started yet)
    // and ahead of InitLog(), so a crash there produces zero debug.log
    // output and surfaces to the injector purely as LoadLibrary/thread
    // failing with 1114. LazyFn below makes each of these a trivially
    // constructed (zero dynamic init) callable that only resolves+scans
    // on its first actual call site invocation -- by which point DllMain
    // has already returned, MainThread is running, InitLog() has already
    // run, and Resolve() itself is now SEH-guarded (see PatternTable.hpp)
    // so even a bad scan degrades to E.Fallback instead of crashing.
    template <typename FnPtr, uintptr_t(*Resolver)()>
    struct LazyFn
    {
        template <typename... Args>
        auto operator()(Args&&... args) const
        {
            static FnPtr Cached = reinterpret_cast<FnPtr>(Resolver());
            return Cached(std::forward<Args>(args)...);
        }
    };

    inline LazyFn<uintptr_t(*)(int, const char*, ...), &Roblox::Patterns::Print> Print;
    inline LazyFn<void(__fastcall*)(lua_State*), &Roblox::Patterns::LuauExecute> Luau_Execute;
    inline LazyFn<lua_State*(__fastcall*)(uint64_t, uint64_t*, uint64_t*), &Roblox::Patterns::GetLuaStateForInstance> GetLuaStateForInstance;
    inline LazyFn<uint64_t(__fastcall*)(uint64_t, YieldState*, YieldingLuaThread**, uint32_t, uint8_t, uint64_t), &Roblox::Patterns::ScriptContextResume> ScriptContextResume;
}

// Dont forget to update Encryptions and Structs