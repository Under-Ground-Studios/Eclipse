# Fix: restored the real vendored Luau interpreter (was delegating to Roblox's native luau_execute) -- PulseExecutor-Dev14, rebranded YuB-X-Module -> Pulse

Root cause, confirmed by two independent working-executor authors: nobody
calls Roblox's real native `luau_execute` directly. `lvmexecute.cpp` had
been gutted at some point in this project's history -- 387 lines instead
of upstream's ~3800, with the entire opcode dispatch loop (every
`CASE_LOP_*` handler, the whole `VM_DISPATCH_TABLE` machinery) replaced
by a one-line stub: `void luau_execute(lua_State* L) { Roblox::Luau_Execute(L); }`.
That's why every attempt to get past the interpreter-dispatch stage this
session kept crashing in unpredictable ways (null-RIP DEP-EXECUTE faults,
0x100000000 fault addresses consistent with 32-bit-vs-64-bit corruption)
-- we were feeding OUR OWN stack values into ROBLOX'S REAL native
function, which has calling-convention/context requirements no amount of
struct-layout correctness can satisfy from outside Roblox's own call chain.

Verified every other VM source file (lvmutils.cpp, lvmload.cpp, ldo.cpp,
lstate.cpp, lfunc.cpp, lapi.cpp, lgc.cpp) matches upstream luau-lang/luau
line-for-line exactly -- only lvmexecute.cpp was touched. Matched this
vendored tree's exact FFlag set (LuauDirectFieldGet, LuauClosureUsageCounter,
DebugLuauUserDefinedClassesRuntime, LuauCallFeedback, LuauYieldIter2, no
LUAU_FLAGVERSION macro, no lvector.h include) and Bytecode.h's
LBC_VERSION_MAX=11 against luau-lang/luau's tag history -- both match tag
0.723 exactly. Restored lvmexecute.cpp from that tag verbatim, minus the
`Closure::usage` reference-counting feature (our vendored Closure struct,
confirmed correct against a real 2026 dump, has no `usage` field -- those
12 call sites were guarded no-ops under `FFlag::LuauClosureUsageCounter`,
which defaults false in this build anyway, so removing them is behaviorally
identical to how they'd already execute). Also independently found and
fixed a real missing-braces bug in `luau_setupcci`/the inline call-return
path (`if (FFlag::LuauClosureUsageCounter) L->base = ...;` -- C++ binds an
unbraced if to the very next statement, so `L->base` was silently never
being set at all, guaranteed VM corruption regardless of the interpreter
delegation bug).

Structural verification before the fix: independently confirmed `TValue`
(Value/extra/tt, 16 bytes), the `lua_Type` enum (0-16, LUA_TINTEGER/
LUA_TVECTOR included), `Closure` (CommonHeader+preload/stacksize/
nupvalues/isC+pad, gclist@0x08, env@0x10, union c/l starting @0x18), and
`UpVal` (CommonHeader+markedopen+pad, v@0x08) all already matched a
struct dump provided independently, byte-for-byte -- ruling out layout
mismatch as the cause before looking at the interpreter body itself. Also
confirmed the `_ENC` field-obfuscation wrappers (Roblox/EncryptionsHelper.hpp's
VMValue1-4 -- real per-build XOR/relative/rotate encoding, not decorative)
are transparent C++ operator overloads: any code doing a normal field
read/write (`L->stacksize`, `cl->c.cont`) auto-encodes/decodes with zero
special-casing needed, so the restored interpreter's normal field access
patterns "just work" against Roblox's obfuscated field layout with no
adaptation required.

Consequence: opcode scrambling (the entire `Offsets::OpcodeLookupTable`
saga -- signature scanning, brute-force multiplier discovery, literal
byte-signature matching, all of it) is now dead code. It only ever existed
to make our bytecode digestible by ROBLOX'S real interpreter. Our own
restored interpreter dispatches on real, unscrambled `LOP_*` values
matching upstream Luau exactly -- `BytecodeEncoder`/`GetOpcodeTableResolution`
etc. are `#if 0`'d out in Execution.cpp, `CompileScript` uses `Luau::compile`'s
default (identity) encoder.

Live-verified (2026-08-26 22:41): `SetupExploit` succeeds, `CreateAndRunThread`
returns normally, and `print("Pulse successfully loaded")` (dllmain.cpp's
own connectivity-check script) actually printed in the live game console --
`22:41:25 -- Pulse successfully loaded.` This is the first real, confirmed
script execution this entire session. A separate, pre-existing delayed
crash (~2s after execution, WRITE fault at a small offset, RSI=0/RDI=0xFFFFFFFF
-- the same signature seen intermittently all session, unrelated to
tonight's fix) still occurs and is NOT yet resolved -- next thing to
investigate.

Also this pass: full rebrand from YuB-X-Module to Pulse in a new sandbox
(PulseExecutor-Dev14, copied from Dev12) -- project directory, .vcxproj/
.vcxproj.filters/.vcxproj.user (RootNamespace, ProjectName, the unused
YUBXMODULE_EXPORTS preprocessor define), the `identifyexecutor()`/
`getexecutorname()` strings, HTTP `Exploit-Identifier`/`User-Agent`/
`*-Fingerprint` headers, the `YuBX_ExecutorClosures`/`YuBX_HookGuards`
Lua registry keys, the on-disk `%AppData%\...\YuB-X\workspace` folder
name, dllmain.cpp's own connectivity-check script string, and every
`YuB-X-Module`-referencing comment in the Client's TypeScript source.
Deliberately NOT renamed: the compiled output filename `Module.dll`
(`TargetName` in the .vcxproj) -- the Client's injector/resources path
depends on that exact name, and renaming it would require coordinated
Client-side changes outside tonight's scope; nothing about it is
YuB-X-branded, it's a generic name. Full clean rebuild + `npm run dist`
verified working after the rename, MD5-matched end to end.

# Fix: RobloxExtraSpace copied whole from RobloxState (was mostly empty)

Disabling InitializeHooks (below) didn't change anything -- exact same
error, meaning it isn't the __index patch: Roblox's own, completely
unmodified __index throws the same "invalid argument #2 (string
expected, got function)" on `game.GetService` for a totally fresh thread.

Real cause: `RobloxExtraSpace` (lua.h) has ~0x70 bytes of unlabeled
padding between its known fields (SharedExtraSpace, Capabilities,
Identity, Script, Actor). The previous fix only ever set
SharedExtraSpace/Capabilities/Identity on a freshly `new`'d, otherwise
all-zero struct -- but Roblox's own internal property dispatch (what
actually resolves `game.GetService`) very plausibly reads more of that
struct than those three fields, and a thread Roblox itself created
(RobloxState) has all of it populated correctly via the cb.userthread
callback we had to disable earlier this session.

Fix: `*ExtraSpace = *RobloxState->userdata;` -- a normal C++ struct copy
(RobloxExtraSpace has no user-declared copy operator, so this is a
compiler-generated memberwise assignment, not a raw memcpy -- Script/Actor
`std::weak_ptr` members get copied through their own real copy-assignment
operator, safe/atomic, no UB) instead of building an empty shell.
SetThreadCapabilities still overrides Capabilities/Identity afterward to
what we actually want.

Kept InitializeHooks disabled for this test to isolate the variable --
this fix alone should resolve the GetService error regardless.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: stopped blanking the whole shared cb struct -- scoped to just cb.userthread, just around lua_newthread

The 0x14/RSI=0 crash survived every prior fix (real allocator vs
SimpleAlloc, persistent vs short-lived thread, signature scanner fix) --
same address, same registers, every time, a couple seconds after a thread
gets created, never during our own thread/script's actual execution.
Confirmed this run: it happens even with ZERO scripts manually sent (the
client auto-sends two small test/ping scripts on connect, which was
previously misread as "the user ran two scripts" -- explains the earlier
"crashed after two scripts" pattern without invalidating it, just
reframes the trigger as automatic).

Realized `FreallocOverride` was still blanking the ENTIRE `global_State::cb`
struct for the whole duration of `CreateAndRunThreadGuarded` (userdata
alloc + SetThreadCapabilities + all 8 RegisterLibrary calls). `cb` is not
per-thread -- it's shared by every thread against this `global_State`,
including Roblox's own live, concurrently-running game. Zeroing it, even
briefly, zeroes Roblox's real callbacks (`onallocate`, `useratom`,
`interrupt`, `debuginterrupt`, `panic`, ...) for anything else sharing
this state too. A crash unrelated to our own thread's data, immune to
every change tried so far, fits this far better than a bug in our own
setup.

Can't remove the suppression entirely: `cb.userthread(L, L1)` fires
*inside* `lua_newthread` itself (`lapi.cpp`), before the new thread's
`userdata` is set (we only set that after `lua_newthread` returns) --
Roblox's real `cb.userthread` writes into the new thread's `userdata`,
guaranteed null at that exact point, reproducing the original
frealloc-era crash (WRITE fault at `RobloxExtraSpace::Identity`'s real
offset) every time if left alone.

**Fix**: new `UserthreadSuppressor`, scoped to suppress ONLY
`cb.userthread`, and ONLY around the single `lua_newthread` call --
every other real callback stays live for Roblox's own concurrent game the
entire time, including through `SetupEnvironment`'s library registration.
`FreallocOverride` itself no longer overrides anything (kept as an inert
save/restore scaffold).

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: signature scanner was blindly trusting PE headers, crashing on every call

New crash after the persistent-thread fix (8s-delayed crash confirmed
gone -- this is different: near-immediate, different address). Realized
it can't be from user bytecode at all -- every script is currently
REFUSED by IsOpcodeTableSane() before real execution -- so it has to be
from setup overhead itself. The one thing that runs unconditionally on
every refused script: Roblox::Print(), resolved through the exact same
signature scanner that's crashed on literally every single call this
entire session (every `[CRASH] stage=SignatureScan` line in every log so
far), always falling back to Offsets::Print -- same dump, same era as the
now-confirmed-wrong Offsets::OpcodeLookupTable.

Root cause of the scanner itself crashing, finally found: `GetModuleBounds`/
`GetSectionBounds` (Roblox/SignatureScanner.hpp) trusted PE header values
(`SizeOfImage`, section `VirtualSize`) as "this many bytes are safely
readable" and scanned straight through with zero verification. Reserved
virtual address space is not the same guarantee as committed+readable
pages -- a module this size has real gaps, and Hyperion (confirmed
earlier this session to actively guard memory in ways plain PE-header
trust doesn't expect) only makes that worse. The crash was completely
deterministic (same relative offset, same register values, every call)
because it's always hitting the same real gap.

Fix: `Scanner::GetSafeSubRanges` now walks the target region through
`VirtualQuery` once, keeps only the sub-ranges that are actually
`MEM_COMMIT` with a readable protection (rejecting `PAGE_GUARD` too), and
`FindPattern` only ever scans (or lets a candidate match span) within one
verified-safe sub-range. This should eliminate the SignatureScan crashes
entirely regardless of whether the patterns themselves turn out to be
correct for this build -- and if they are, this may resolve Print/
OpcodeLookupTable/etc. with real, dynamically-found addresses instead of
the confirmed-stale hardcoded dump values, without needing any more
manually re-dumped offsets at all.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: eliminated persistent exploit thread -- confirmed root cause of the ~8s-later Roblox crash

Confirmed this crash is unrelated to allocator choice: identical crash
address (`RobloxPlayerBeta.exe`, WRITE fault) reproduced with BOTH the real
`frealloc` (previous entry) AND the earlier `SimpleAlloc` override, same
~8 second delay after `SetupExploit` succeeded either way. That rules out
the GC/allocator-ownership theory -- there's no malloc'd memory involved
when using the real allocator, yet the exact same crash happens.

Real cause: `SharedVariables::ExploitThread` was created ONCE and kept
alive for the rest of the process's lifetime -- a foreign thread
permanently linked into Roblox's live GC thread list that Roblox's own
internal bookkeeping never expected to exist indefinitely.
`Execution::ExecuteScript` already safely creates a short-lived sub-thread
per script run and pops it immediately after (`lua_newthread` + `lua_pop`)
without ever triggering this -- the crash was specifically about
persistence, not creation.

**Fix**: removed the persistent thread entirely. `SetupExploit` now only
caches `SharedVariables::RobloxState` (Roblox's own real, permanently-alive
ScriptContext thread -- safe to hold a raw pointer to) and sets
`ExploitReady`. Everything that used to happen once and stick around
(`RobloxExtraSpace` alloc, `SetThreadCapabilities`, `Environment::SetupEnvironment`)
now happens fresh in `TaskScheduler::CreateAndRunThreadGuarded`, called by
the new `TaskScheduler::PollAndExecute()` for every queued script: create
a thread off `RobloxState`, set it up, run the script, `lua_pop` it
immediately after -- exactly the same safe pattern `ExecuteScript`'s own
inner sub-thread already used, just extended one level up. Costs
re-registering the 8 libraries per script run instead of once, in exchange
for not leaving anything permanently alive in Roblox's live thread list.

Also fixed a regression this introduced: `Miscellaneous::getgenv()` used
to reach across to the persistent `SharedVariables::ExploitThread`
specifically (now always null) to return a stable shared environment
table -- simplified to just return the calling thread's own
`LUA_GLOBALSINDEX`, which already contains everything `RegisterLibrary`
registered via the `luaL_sandboxthread` proxy chain, no separate thread
needed.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Architecture change: execution driven natively from PresentHook, game/RunService dependency removed entirely

Root cause of `game`=type9 (LUA_TTHREAD) confirmed on a genuinely fresh
process (not corruption, not our thread's fault -- identical on
RobloxState, Roblox's own untouched thread): `Roblox::GetLuaStateForInstance`
does not return a normal running script's thread. Whatever it actually is
(never fully identified -- see below), `game`/`script`/`workspace` were
never bound on it the way they are on a real LocalScript's thread. There
was no way to make `game:GetService(...)` work from this thread; the offset
itself may be fine for what it does (a valid, callable lua_State) but it
isn't "a real script's thread."

Research (open-source executors, e.g. the publicly documented Xeno
architecture) confirmed the actual fix isn't finding a way to make `game`
work -- real executors don't depend on it either. `SetupExecution`'s only
job was getting a "run queued scripts every frame" hook via
`RenderStepped:Connect`. `PresentHook` (this session's earlier work) IS
ALREADY exactly that: a real "every frame, on Roblox's own thread" hook,
native. `ScriptsHandler` never actually used its own `lua_State` parameter
-- it always operated on `SharedVariables::ExploitThread` directly, and
`Yielding::RunYield()` takes no arguments -- so it's a plain C++ function,
safe to call directly every frame from `PresentHook`.

**Removed entirely**: `SetupExecution` (the `game:GetService("RunService")
.RenderStepped:Connect(ScriptsHandler)` Lua-level hook) and the `game`
diagnostic comparison block. **Added**: `SharedVariables::ExploitReady`
flag (set true once `SetupExploit` finishes `SetupEnvironment`) and
`TaskScheduler::PollAndExecute()` (the same drain-one-queued-script logic
`ScriptsHandler` always ran, called directly, guarded, every frame from
`PresentHook`). This removes the `game`-binding gap from the execution
path completely -- nothing in the working pipeline touches `game` anymore.

**Still open, flagged honestly, not silently ignored**:
- **What `GetLuaStateForInstance` actually returns** was never fully
  identified -- only that it's a valid, callable thread that isn't a real
  script's thread. Irrelevant to basic script execution now that `game`
  isn't needed, but would matter for any future feature that needs a real
  Instance (e.g. `game.Players`, `workspace`, spawning real Instances).
- **The `FreallocOverride` GC/allocator ownership risk** (objects
  allocated via our own malloc-backed `SimpleAlloc` linked into Roblox's
  real, shared GC lists) is real and was confirmed this session via the
  VEH catching a crash inside `RobloxPlayerBeta.exe` on an unrelated
  thread, consistent with Roblox's own GC later freeing one of our
  objects through its real (mismatched) allocator. It's structurally
  mitigated now, not eliminated: a *successful* `SetupExploit` keeps
  `SharedVariables::ExploitThread` referenced for the process's lifetime
  (Roblox's GC won't consider it garbage), and removing the `game`
  dependency should make `SetupExploit` succeed on the first real attempt
  instead of failing-and-abandoning-a-thread every second the way it did
  all session -- but a failed attempt still leaks one thread with no
  cleanup. Not addressed further this pass.
- `SignatureScan`'s own guarded exception (seen in every log, harmless,
  falls back to the hardcoded offset) is understood and expected --
  documented earlier this session, not a new open item.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Test: InitializeHooks disabled -- pinpointed to the __index patch itself

The isC safety check (below) didn't change the observed error -- game's
__index/__namecall genuinely are C closures, so patching proceeded both
times. Added per-call diagnostics to SetupExecution and got a precise
localization: `game type=9` logs, but the *next* line (after
`lua_getfield(L, -1, "GetService")`) never printed -- the throw is inside
that one getfield call, which dispatches through the just-patched
__index (IndexHook -> OriginalIndex(L)). Same root pattern as
frealloc/cb.userthread earlier this session: a real Roblox internal
function behaving differently when invoked outside its own exact normal
calling path, even with correct-looking arguments.

Disabled the `InitializeHooks(L)` call in SetupEnvironment for this pass
(commented out, not deleted) to unblock the core script-execution
pipeline first -- __index/__namecall interception (blocking
HttpGet/UnsafeFunction names) is a security-hardening feature, not
required for basic script execution. Revisit as its own separate problem
once RenderStepped/print is confirmed working.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: InitializeHooks corrupting game's real __index/__namecall closure

First run past the WARP fix (below) got all the way through SetupEnvironment
and into SetupExecution -- furthest yet. New error there:
`"invalid argument #2 (string expected, got function)"`, then the *next*
retry hard-crashed with `STATUS_STACK_OVERFLOW` (0xC00000FD). That
combination -- a garbled, unrelated-looking Lua error immediately followed
by a stack overflow -- pointed at memory corruption, not a real argument
mistake.

Found it in `Environment.cpp`'s `InitializeHooks`: it hot-patches game's
real `__index`/`__namecall` metamethod by writing directly into
`Closure::c.f` (the union member for a C closure's function pointer) --
with no check that the closure is actually a C closure first, and while
also accepting `LUA_TLIGHTUSERDATA` (for which `clvalue()`, valid only for
`LUA_TFUNCTION`, reinterprets an unrelated raw pointer as if it were a
`Closure*`). `Closure::c.f` and `Closure::l.p` (a Lua closure's real
`Proto*`) share the same union slot at offset 0x18. If game's real
`__index`/`__namecall` turned out to be a Lua closure rather than a C one,
"patching c.f" actually stomped that closure's real `Proto*` with
`IndexHook`'s raw address -- permanently corrupting a live, shared engine
object, which matches both symptoms exactly (garbled follow-on error, then
a stack overflow from something later calling the corrupted closure and
recursing on garbage).

Fix: only `LUA_TFUNCTION` (dropped the `LUA_TLIGHTUSERDATA` branch
entirely), and only patch if `Closure::isC` is actually true; otherwise
log and skip hooking for this pass rather than corrupt memory.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: PresentHook's dummy device crashes Roblox on game join (WARP driver)

New symptom, unrelated to the Lua-side fixes above: injecting in the menu
was fine, but joining any game mode crashed Roblox outright (no error
dialog, just closed/froze) -- no [CRASH] log line at all, meaning it
happened outside anything our own SEH guard can see.

Root cause: `Hook::InstallPresentHook` (`PresentHook.hpp`) creates a
disposable D3D11 device with `D3D_DRIVER_TYPE_HARDWARE`, on `MainThread`
(a foreign OS thread, not Roblox's own), specifically to read the real
`Present` vtable slot. That call happens right as `IsInGame` flips true --
i.e. right as Roblox's own hardware D3D11 device transitions from idle
(menu) to actively rendering the 3D world for the first time. Creating a
second hardware device against the same GPU/driver from a different
thread at exactly that moment is a known trigger for driver-level
contention crashes.

Fix: `D3D_DRIVER_TYPE_WARP` instead of `_HARDWARE`. We only need the
`Present` vtable slot's address, which is identical regardless of driver
type (determined by the DXGI/D3D11 runtime interface, not the GPU
backend) -- WARP is a pure software rasterizer, never touches the real
GPU/driver, so it can't contend with Roblox's real device.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: real Lua error was hitting SEH as a fake "crash" -- now caught + fixed

Past the userdata fix (below), SetupEnvironment started throwing
exceptionCode=0xE06D7363 (the MSVC C++-exception magic number, not a real
hardware fault) inside DebugLib::RegisterLibrary, caught by Debug::Guard's
SEH __except and reported as a generic crash -- the actual error message
was being thrown away. Root cause of *that*: this build's Luau (ldo.cpp)
propagates Lua-level errors via real `throw lua_exception(...)`, not
setjmp/longjmp -- normally caught by `luaD_rawrunprotected` inside a
`lua_pcall` boundary. `SetupEnvironment`/`RegisterLibrary` call raw C API
functions directly with no `lua_pcall` around them, so any ordinary
Lua-level error along the way had nothing to catch it.

Added a real `try/catch(std::exception&)` around `SetupExploit` in
`SetupExploitGuardedOnRobloxThread` (safe: this callback runs in its own
stack frame, not `Debug::Guard`'s own `__try` frame, so the C2712
restriction on RAII/exceptions doesn't apply here) -- this finally surfaced
the actual message: `"attempt to modify a readonly table"`.

**Real bug**: `ExploitThread` shares Roblox's real, live globals table
(`luaE_newthread`: `L1->gt = L->gt`) -- including its existing `debug`
table, which Roblox's own sandboxing marks readonly, same protection any
real script would hit. `DebugLib::RegisterLibrary` was writing new fields
directly into that shared, protected table object. Fixed the same way
`luaL_sandboxthread` itself handles the globals table: build a fresh,
writable `debug` table with `__index` proxying to Roblox's real one (so
`debug.traceback` etc. still resolve), then rebind the global name
`"debug"` to point at ours instead of mutating Roblox's.

Also fixed a related bug this surfaced: `SetupExploitGuardedOnRobloxThread`
was swallowing the exception without propagating failure, so
`PresentHook`/`MainThread` reported "SetupExploit succeeded" even when it
aborted halfway through and `InitializeHooks`/`SetupExecution` (the
RenderStepped hook queued scripts need to ever run) never happened.
`SharedVariables::ExploitInitSucceeded` is now set explicitly by the
catch block and by `SetupExploit`'s own return value, not inferred from
"Guard() didn't hard-crash."

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: allocate ExploitThread->userdata ourselves (RobloxExtraSpace)

First real progress past lua_newthread: with cb blanked (below), `stage=
lua_newthread done` printed for the first time this whole investigation --
but the very next line, `SetThreadCapabilities` (`L->userdata->Identity =
Level`), WRITE-faulted at address `0x60` exactly. `RobloxExtraSpace::Identity`
(lua.h) sits at offset `0x60` -- a literal `NULL->Identity` write. Root
cause: `preinit_state` (lstate.cpp) unconditionally sets a brand-new
thread's `userdata` to NULL; normally Roblox's own `cb.userthread`
callback allocates the real `RobloxExtraSpace` and assigns it right after
`lua_newthread` returns -- the same callback we just had to blank to stop
it crashing (previous entry), so that allocation stopped happening for
our thread too.

Fix: `TaskScheduler::SetupExploit` now allocates its own `RobloxExtraSpace`
right after `lua_newthread` (`new RobloxExtraSpace()`, never freed --
lives for the process's lifetime like every other exploit-global object
here), copying `SharedExtraSpace` from `RobloxState->userdata` (Roblox's
own, real, already-initialized ScriptContext thread -- `Shared*` is meant
to be shared across sibling threads of the same ScriptContext, so this is
safe to read). `Script`/`Actor` default-construct to empty `weak_ptr`s,
which is exactly what `IndexHook`/`NamecallHook`'s `Script.expired()`
check wants for an unrestricted exploit thread.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: identified real crash site as cb.userthread, not frealloc -- override extended

The frealloc override (below) didn't stop the crash -- same exact address,
even with frealloc swapped to our own allocator. That proved the crash was
never inside frealloc at all: RIP=0x...82B3 is 0x1143 bytes away from
frealloc's real entry (0x...7170), just close enough in the compiled
binary to look related. Traced it precisely: `lua_newthread()` in our own
vendored `Dependencies/Luau/VM/src/lapi.cpp:214` calls
`g->cb.userthread(L, L1)` if Roblox has one registered -- and confirmed
Roblox does (RCX==RobloxState at the crash matches userthread(L, L1)'s
exact first-arg signature). Same class of bug as frealloc: a real,
Roblox-registered callback that assumes it's only ever invoked from
Roblox's own call chain.

`FreallocOverride` now also saves/blanks the whole `lua_Callbacks` struct
(`global_State::cb` -- userthread, onallocate, useratom, interrupt,
debuginterrupt, panic, debugbreak, debugstep, debugprotectederror) for the
duration of SetupExploit, not just userthread specifically -- our own
vendored VM code calls several of these during totally normal operation
(useratom on every new string, interrupt during instruction dispatch), so
blanking the whole struct once heads off the same crash shape recurring
field-by-field. Restored via the same RAII destructor as frealloc.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: stopped calling Roblox's real frealloc directly (own allocator override)

Follow-up to the Present-hook fix below: moving SetupExploit onto Roblox's
own thread didn't fix the crash -- same exact address, every time, even
now genuinely running on Roblox's own render thread. Added real crash
forensics to SafeCall.hpp's SEH filter (full CONTEXT register dump +
access-violation type/faulting-address, not just exception address) to
stop guessing. That showed: RIP was 0x1143 bytes *inside* frealloc's own
body (real function, really called successfully) when it WRITE-faulted at
address `0x58` exactly -- null pointer + offset 0x58, which is exactly
where our own `lua_State::userdata` field sits. Roblox's real allocator
writes memory-category bookkeeping into what it thinks is "the currently
executing thread", tracked through some context only Roblox's own
interpreter sets up before calling the allocator -- calling frealloc
directly from outside that call chain leaves it null. Not an offset bug;
an unmet invariant we can't satisfy from outside Roblox's own code path.

Fix: `TaskScheduler.cpp`'s new `FreallocOverride` (RAII) temporarily swaps
`global_State::frealloc`/`ud` to a plain realloc-backed allocator
(`SimpleAlloc`) for the full duration of `SetupExploit` -- every GC
allocation we make (lua_newthread's page, every lua_newtable/
lua_pushcclosure in SetupEnvironment) goes through code with no hidden
invariants, then the real allocator is restored before returning control
to the game. Safe because we own the entire lifecycle (alloc and eventual
GC free) of anything allocated through our own vendored VM code -- Roblox's
own scripts/objects never see the swap.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: SetupExploit moved off MainThread onto Roblox's own thread (Present hook)

Follow-up to the global_State fix below: injection stopped crashing the
game, but SetupExploit was still access-violating deterministically every
retry (address inside RobloxPlayerBeta.exe itself, at frealloc -- correctly
resolved this time, just called from the wrong thread). Root cause:
SetupExploit ran directly from MainThread, a foreign OS thread Roblox never
created; Roblox's own render thread was concurrently touching the same live
lua_State/global_State with zero synchronization -- a data race, not a
struct-layout bug.

First attempt: inline hook (hand-rolled 14-byte jmp trampoline,
`Exploit/Hook/InlineHook.hpp`) on the real `ScriptContextResume` (called
continuously by Roblox's own task scheduler, on Roblox's own thread).
`VirtualProtect` on that address failed with `GetLastError()==1655` even
though `VirtualQuery` confirmed the region was valid/committed/executable --
Hyperion actively blocks writes to RobloxPlayerBeta.exe's own code pages.
Confirmed via 2026 sources: Hyperion (Roblox's anti-tamper, ex-Byfron) is
known to actively guard exactly this.

Replaced with a vtable-swap hook on `IDXGISwapChain::Present`
(`Exploit/Hook/PresentHook.hpp`) instead -- called by Roblox's own render
thread every frame. The vtable lives in Microsoft's own d3d11.dll/dxgi.dll,
not Roblox's module, so patching one pointer in it doesn't touch anything
Hyperion guards. `TaskScheduler::InstallExecutionHook` (renamed from
`InstallResumeHook`) creates a disposable, invisible D3D11 device+swapchain
purely to read the real (shared, class-level) vtable, patches slot 8
(Present), and tears the disposable device down immediately -- the patch
applies to Roblox's real swapchain too since it's the same shared vtable
regardless of creation order. `dllmain.cpp`'s MainThread now hands
SetupExploit off via `SharedVariables::PendingDataModel`/`ExploitInitPending`
instead of calling it directly; `PresentHook` runs it once, on Roblox's own
thread, the next frame.

Linked `d3d11.lib`/`dxgi.lib`. Rebuilt (MSBuild Release|x64, exit 0) and
repackaged (`npm run dist`, exit 0).

# Fix: global_State applied from real dump (message(1).txt)

Follow-up to the crash below this entry (`SetupExploit`/`lua_newthread`
crashing with 0xC0000005): traced to `global_State` (`lstate.h`) never
having been part of any offset dump for this build — the vendored struct
was stock-old-upstream Luau, field order and even field set completely
different from what a live build from ~Aug 2026 actually has. A test pass
in `PulseExecutor-Dev13` (sandbox copy) first tried substituting current
upstream `luau-lang/luau` master's `global_State` as a best-effort
approximation — got past the crash (no crash on inject/join anymore) but
wasn't independently confirmed against a real dump.

This pass applies the **actual dump** for this build
(`Downloads/message(1).txt`, `global_State` + `stringtable`, with explicit
`_padN` gap fields) verbatim to `lstate.h` — `frealloc`/`ud` (what
`luaM_newgco` reads on every single GC allocation) are now at `0x28`/`0x30`.
Also added `lua_EmbedderGc`/`lua_EmbedderMark` typedefs to `lua.h` (needed
for the `embeddergc` field; didn't exist in this vendored copy before).
`udatamark` is a plain function-pointer array in this dump (not the
`lua_UserdataMark` typedef used in the Dev13 upstream-master pass) — kept
as given rather than introduced unprompted. This build's `global_State`
does **not** have `builtinPcall`/`builtinXpcall`/`ptrenckeynew`/
`ptrencactive` that current upstream master has — confirms this build
predates whatever Luau revision added those, and confirms the Dev13
upstream-master substitution, while it stopped the crash, wasn't the same
layout as this build's real one.

Rebuilt (MSBuild Release|x64, exit 0) and repackaged (`npm run dist`,
exit 0).

# Fix: injection failing with 1114 (ERROR_DLL_INIT_FAILED)

Reported symptom: injector reports success, other DLLs inject fine, but
Module.dll fails with Windows error 1114 and no `debug.log` is ever
created (meaning `MainThread`'s `InitLog()` — its first line — never ran).

**Root cause**: `Offsets.hpp`'s `namespace Roblox { inline auto Print = ...;
... }` block (added this session as part of the Tier 1.1 signature-scanner
work) called `Roblox::Patterns::Print()` / `LuauExecute()` /
`GetLuaStateForInstance()` / `ScriptContextResume()` directly in each
global's initializer. Namespace-scope `inline` variables with a
non-constant initializer are still dynamically initialized like any other
global — for a DLL that means "runs during `DLL_PROCESS_ATTACH`'s global
constructor phase, before `DllMain`'s own body executes." That ran a full
AOB byte-scan over the host process's mapped image
(`Patterns::Resolve` -> `Scanner::FindPatternInModule`) as part of static
init, completely unguarded (`Debug::Guard` was only ever wired to run
*inside* `MainThread`, which hadn't started yet) and ahead of `InitLog()`.
Any failure there — an access violation from a bad `SizeOfImage` read, a
pattern miss path that still touched unmapped memory, anything — is fatal
during CRT init and the loader reports it back as `LoadLibrary` failing
with `ERROR_DLL_INIT_FAILED`, before this module's own `DllMain` or
`MainThread` code ever gets a chance to run or log.

**Fix**, two parts:
1. `Roblox::Print`/`Luau_Execute`/`GetLuaStateForInstance`/
   `ScriptContextResume` in `Offsets.hpp` are now `LazyFn<...>` — a
   trivially-constructed (zero dynamic init) callable that only resolves
   +scans on its *first actual call site invocation*, long after
   `DllMain` has returned and `MainThread`/`InitLog()` are already
   running. Call-site syntax (`Roblox::Print(3, "%s", ...)` etc.) is
   unchanged.
2. `PatternTable.hpp`'s `Resolve()` now runs the scan itself through
   `Debug::Guard` (SEH `__try/__except`), same pattern already used for
   `GetDataModel`/`SetupExploit` in `dllmain.cpp` — a bad scan now
   degrades to `E.Fallback` (the original hardcoded `REBASE()` constant)
   and logs `[CRASH] stage=SignatureScan ...` instead of crashing.

Rebuilt (`MSBuild Release|x64`, exit 0) and repackaged
(`Client`: `npm run dist`, exit 0) — `Client/release/PulseExecutor.exe`
and `Client/release/win-unpacked/resources/engine/Module.dll` both carry
the fixed binary (md5 `03a0ebe8675356c180e5749cf3b30df6`).

# YuB-X-Module dev pass 9 — real offsets/struct layout applied

Base: verbatim copy of `PulseExecutor-Dev8\YuB-X-Module`, confirmed building
clean before this copy was made. Applies the offset/struct dump supplied
for Roblox client version `ddf602d9cfe44005` (verification date not given
by the source; per explicit instruction, applied anyway — a version
mismatch would surface as a crash or corrupted read at runtime, not
something catchable here).

**Scope, explicitly**: this pass applies the *engine* offsets/struct
layout (`Offsets.hpp`, `Encryptions.hpp`, `lobject.h`, `lstate.h`). The
anti-tamper/injector offset section of the same sheet was explicitly
excluded per instruction — not touched.

## What was applied

- `Offsets.hpp`: `Print`, `OpcodeLookupTable`, `ScriptContextResume`,
  `GetLuaStateForInstance`, `Luau_Execute`, `NilObject`, `DummyNode`,
  `FakeDataModelPointer`, and the `DataModel`/`ExtraSpace` struct-field
  offsets — all replaced with the new build's values.
- `Encryptions.hpp`: every `VMValueN` scheme mapping changed (all 14
  fields moved to a different scheme than the previous build).
- `lobject.h`/`lstate.h`: `CommonHeader` field order (`tt` now first, was
  `memcat`), and the full field layout of `Proto` (216 bytes, was a
  different size), `Closure` (72 bytes), `TString` (cosmetic rename only),
  `LuaTable` (48 bytes, reordered), and `lua_State` (128 bytes, every
  field after the first four reordered) — all rewritten to match the new
  dump's confirmed offsets exactly. `UpVal`/`CallInfo` were not in the
  supplied data and were left unchanged.

Every field kept the same C++ name it already had (only offsets/order/
encoding scheme changed), so nothing in `Closures.hpp`/`Debug.hpp`/
`Metatable.hpp`/`Reflection.hpp` needed to change — field access by name
means the compiler recomputes the correct offsets automatically once the
struct definitions are right.

## Two real conflicts found and resolved, not silently picked

**`Closure::usage` — the supplied dump has no room for it (72-byte total,
every byte accounted for), but this vendored VM source's own interpreter
still actively increments/decrements/asserts on it at ~15 call sites**
(`lvmexecute.cpp`, `ldo.cpp`, `lstate.cpp`, `lfunc.cpp`, `lvmutils.cpp`) —
removing the field outright didn't just fail to compile, it would have
meant either keeping it (breaking the confirmed 72-byte live-process
layout that matters for reading real game closures via hooks) or dropping
it (breaking this source's own bytecode interpreter). Checked whether the
resulting count is ever actually *read* anywhere beyond those
increment/decrement/assert sites — it isn't. It's dead bookkeeping:
written, asserted, never consumed. Commented out all ~15 call sites rather
than keep a field that doesn't fit the confirmed live layout; `LUAU_ASSERT`
is release-build-inert anyway, so this changes nothing observable even for
the module's own self-allocated closures.

**`Closure::c.debugname` — the dump described it as a plain, unencoded
`TString*`, but `lapi.cpp`'s `lua_pushcclosurek` and `lclass.cpp` both do
`cl->c.debugname = someConstCharPtr;` directly** (a real `const char*`
assignment, e.g. `constructor->c.debugname = "luaR_createobject";`) — a
plain `TString*` field can't compile against that. Kept the field at the
dump's offset (0x20) but as the encoded `const char*` type this vendored
source actually requires; the second 8-byte slot the dump described stays
reserved (offset-preserving, unread) so total size and every later
field's offset still match the dump exactly.

Both are genuine, checkable discrepancies between the supplied dump and
what *this specific vendored Luau source* needs to compile and run
correctly — not places where the dump was blindly trusted or blindly
overridden without checking which side actually mattered.

## Empirically re-verified, not just compiled

The standalone encoding-verification harness from an earlier pass
(`EncodingTest/`, independent of Roblox, patches only the two GC
singletons needed to run this Luau build standalone) was rerun against
the new struct layout and new `VMValueN` mapping — `PROTO_SOURCE_ENC` is
now `VMValue1` at offset `0x18` (was `VMValue4` at `0x8`). Result:
```
source->data = "TestChunk"
chunkname readable: YES - MATCH
```
Confirms the new struct+encoding combination decodes correctly together,
not just that it compiles.

## What this does NOT verify

Everything above was checked by compiling (real, clean build) and by the
standalone harness (real, passing). Neither exercises the actual
Roblox-address offsets (`Print`, `ScriptContextResume`,
`GetLuaStateForInstance`, `Luau_Execute`, `FakeDataModelPointer`, etc.) —
those can only be verified by actually injecting into a live Roblox
process, which this environment can't do. If the supplied client version
doesn't match what's actually running, expect a crash or silently
corrupted reads at those specific call sites, not a build-time signal.
