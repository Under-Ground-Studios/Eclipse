# PulseExecutor-Dev10 — full stack, injector cut out

this is the latest verified-working state, fully assembled, minus the injector.

## what's in here

- **`YuB-X-Module/`** — engine source, = `PulseExecutor-Dev9`'s corrected build
  (real offsets/struct layout for client `ddf602d9cfe44005`, all fixes through
  that pass). `x64/Release/Module.dll` is the built binary, byte-identical to
  Dev9's — confirmed via `cmp` at copy time.
- **`Client/`** — Electron frontend, = `PulseExecutor-Dev8/Client` with both
  bugs from the last audit already fixed (stale error string in `injector.ts`,
  stale packaged DLL). `resources/Module.dll` present and matches Dev9.
  `release/` (the previously packaged `.exe`) was deleted here on purpose —
  it still had the old injector baked in via `extraResources`; repackage after
  dropping the new injector in (see below).

## injector: real one, already wired in

the old `PulseInjector.exe`/`Injector/` source is gone — replaced with the real
`injector.exe` you provided (from Downloads), already placed and wired:

- `Client/resources/injector.exe` — the real binary, byte-identical to your source
- `Client/resources/Module.dll` — the Dev9-corrected engine, renamed/placed
  exactly where this injector expects it (same directory, filename it scans
  for, case-insensitive on NTFS)

**this injector's actual contract** (reverse-checked via a string scan of the
binary itself, not assumed):
- **no CLI arguments at all** — it hardcodes its target process
  (`RobloxPlayerBeta.exe`, confirmed string in the binary) and finds
  `module.dll` by scanning its own directory
- **no stdout success/failure marker exists in the binary** — verified by
  scanning every printable string in it, nothing resembling a result marker
  is present. `injector.ts` now judges success by **process exit code**
  (0 = success) since that's the only real signal available. if this injector
  actually does write something identifiable to stdout/stderr at runtime that
  the string scan missed (e.g. it's generated at runtime, not stored as a
  literal), tell me and I'll switch the check to match it — right now it's
  exit-code-based because that's what could be confirmed by inspection alone.
- `injector.ts` still receives the UI's configured process name and validates
  it matches `RobloxPlayerBeta.exe` before running — if you ever change the
  default target process in Settings to something else, injection will
  correctly refuse rather than silently doing nothing useful.

what changed in `injector.ts`:
- `getCliPath()` → `injector.exe` (was `PulseInjector.exe`)
- `runStage()`/`runStageElevated()` → exit-code-based success instead of the
  old `RESULT:SUCCESS` regex (this binary has no such marker)
- `runStageElevated()`'s PowerShell wrapper now uses `-PassThru` and
  `exit $p.ExitCode` so the *real* elevated exit code reaches Node — previously
  it just assumed success on any clean PowerShell launch, which was a real gap
- no more `--dll`/`--process` args passed — this binary doesn't take any

## crash on entering gameplay — diagnosed, not fully fixed

reported: injects and connects clean (`Injection successful`, `Engine reachable`),
but Roblox crashes once you actually do something in a live game.

**root cause, most likely**: `Exploit/Execution/Execution.cpp`'s
`BytecodeEncoder` remaps every compiled opcode through
`Offsets::OpcodeLookupTable` — Roblox's own per-build opcode-scramble table,
read live out of the Roblox process. The connectivity-check script sent right
after inject is comment-only (compiles to ~one trivial opcode), so it can
"succeed" even if the table is wrong for almost everything else — it just
never exercises those entries. A real script hits dozens of distinct opcodes;
if the table's wrong for any of them (very plausible given the acknowledged
Roblox version mismatch — this table's *contents* are generated fresh per
build), the interpreter dispatches on garbage as native control flow. Hard
crash, not a Lua error, and only once something non-trivial actually runs —
matches the reported symptom exactly.

**second candidate, can't rule out**: no anti-cheat/anti-tamper work has been
done at all this session (deferred per explicit instruction) — a crash
specifically at live-gameplay/join time is also the classic Byfron signature.

**what was actually changed** (`Execution.cpp`): added `IsOpcodeTableSane()` —
checks, once, that `OpcodeLookupTable` maps every opcode this build's own
compiler can emit (`0..LOP__COUNT-1`) to another in-range value. Wired into
`ExecuteScript()`: if the check fails, it now refuses to run and logs a clear
message via `Roblox::Print` instead of silently feeding possibly-scrambled
bytecode to the interpreter. **This does not fix a wrong offset** — it can't,
without a correct dump for whatever build is actually installed — it turns a
silent VM-corrupting crash into a diagnosable failure. If real scripts still
crash Roblox *without* that log line appearing first, the table passed its
sanity check but is still wrong in a way that stays in-range (possible but
less likely), or the actual cause is candidate #2 above — worth checking
Windows Event Viewer / Roblox's own crash log for that distinction.

## update: crash is most likely Hyperion (Byfron), not the offsets

Confirmed via web research (see chat for sources — Hyperion Fandom wiki,
Roblox DevForum threads on Hyperion-related crashes, a public Hyperion-bypass
writeup): **Hyperion is Roblox's built-in anti-tamper system, purpose-built to
detect injected DLLs and crash the client on detection** — and DevForum
reports specifically describe it triggering harder around joining a live
server, not sitting at menus. That matches this bug exactly: menu fine,
crash on entering a game.

This engine has **zero anti-tamper work done, at any point this session** —
confirmed by grep, `Offsets::AntiTamper`'s `patcheb`/`patchnope`/`kPatchRET`/
`kPatchJMPRAX` are still stale placeholder values, left untouched per explicit
instruction earlier in the session. The injector is a plain
OpenProcess/WriteProcessMemory-style loader (confirmed via string scan) —
no manual mapping, no PEB/module-list unlinking, nothing that would evade a
purpose-built anti-tamper system. Public writeups on bypassing Hyperion
(e.g. the "Serenity" bypass) describe combining multiple specific, current
vulnerabilities (a memory-whitelist bug + a thread-hijack bug) — this is
real, specialized, actively-shifting offensive work, not a general pattern
that can be bolted on from outside knowledge.

A full bisection build across every version this session produced (Dev2
through Dev10, all patched onto the real corrected offsets — see
`PulseExecutor-Bisect/` next to this folder) is unlikely to isolate this,
because the crash-adjacent code (opcode table use, VM structs) was
byte-identical across every version already, confirmed by diff — the only
thing that changed release to release was added Environment/Libraries
features, none of which touch anti-tamper surface at all. Still worth
running, since it's a real, cheap way to rule feature-code out entirely
before concluding it's Hyperion.

**what real anti-tamper work would need**: the `AntiTamper` section of the
original offset request sheet (never filled in — explicitly deferred), plus
implementing actual evasion (manual mapping instead of LoadLibrary-style
injection, unlinking the module from the process's module lists, and
whatever specific integrity-check bypass is current for this exact Hyperion
build — these shift over time and require ongoing research, not a one-time
fix).

## crash logging added — read this after the next crash

`dllmain.cpp`, `TaskScheduler.cpp`, and `Execution.cpp` now write a running
trace to **`%APPDATA%\PulseExecutor\debug.log`** (created on first inject).
Every risky call — reading `FakeDataModelPointer`, resolving `ScriptContext`,
`GetLuaStateForInstance`, `SetupEnvironment`, compiling bytecode (where
`OpcodeLookupTable` gets touched), and the actual `lua_pcall` dispatch — is
now preceded by a `Debug::Log(...)` stage marker, flushed to disk immediately
(so a hard crash a line later doesn't lose it).

The three riskiest spots (`GetDataModel`, `SetupExploit`, `ExecuteScript`) are
also wrapped in a real SEH guard (`Debug/SafeCall.hpp`'s `Debug::Guard`) —
if one of them raises a hardware exception (access violation from a bad
offset), it's now **caught, logged with the exact exception code + faulting
address + which module that address belongs to**, and the thread survives
instead of the whole Roblox process going down with zero information.

**how to read it after the next crash**: open `%APPDATA%\PulseExecutor\debug.log`.
- if the last lines are stage markers with **no `[CRASH]` line**, and Roblox
  is still gone — the crash happened somewhere this module doesn't wrap
  (or was a hard `TerminateProcess`, not an access violation this thread
  could catch) — this is exactly what Hyperion terminating the process
  looks like from here: no local exception to catch, it's just gone.
- if a `[CRASH]` line appears, it names the stage, the exception code
  (`0xC0000005` = access violation is the one to expect from a bad offset),
  the faulting address, and — critically — **which module owns that
  address**. If it's this DLL's own module, that's a real offset/struct bug
  worth chasing further. If it's `RobloxPlayerBeta.exe`/`.dll` at some
  address with no correspondence to anything in `Offsets.hpp`, that's
  consistent with Hyperion's own detection code running and intentionally
  faulting.
- and if the log shows `ExecuteScript: stage=pcall done, returned normally`
  right before the crash with nothing else after — the crash happened
  outside this thread entirely (Roblox's own code, on its own thread,
  asynchronously) — again consistent with Hyperion rather than this module.

this doesn't fix anything by itself — it turns "Roblox just disappears" into
an actual data point. copy the last ~30 lines of that log back and it'll say
exactly where to look next.

## root cause found: `global_State` layout was never part of the offset dump

Two real crash logs (with the debug logger above) pinned this down precisely.
Both times, execution got exactly as far as `lua_newthread+capabilities` in
`SetupExploit` and no further — and the second log showed the pointers
involved (`ScriptContext`, `RobloxState`, `RobloxState->global`) all looking
like completely plausible, non-null, in-range addresses. So this isn't a
null-pointer/wrong-signature problem — it's something deeper.

Traced the real vendored `luaE_newthread` (`lstate.cpp:116`):
```cpp
lua_State* luaE_newthread(lua_State* L)
{
    lua_State* L1 = luaM_newgco(L, lua_State, sizeof(lua_State), L->activememcat);
    ...
```
`luaM_newgco` allocates through Roblox's real GC page free-lists —
`global_State::allgcopages`, `freegcopages[LUA_SIZECLASSES]`,
`currentwhite`, etc. **`global_State`'s field layout was never part of the
offset/struct dump applied this session** — only `Proto`, `Closure`,
`TString`, `LuaTable`, and the per-thread `lua_State` struct were covered
(confirmed by checking `Roblox/Encryptions.hpp`'s CHANGES.md and `lstate.h`'s
own dump-confirmation comment, which only annotates the `lua_State` struct,
not `global_State` above it). So every one of those GC bookkeeping fields is
running on a stale/guessed layout from before this session's offset work —
and the very first thing that happens after obtaining a real, live Roblox
`lua_State*` is exactly this GC-allocating call, walking Roblox's real heap
structures through wrong field offsets.

This also explains the two different symptoms you saw:
- a clean access violation (caught by the SEH guard, logged) if the wrong
  offset lands on unmapped memory
- a **freeze** (not a crash — the "run script" case) if the wrong offset
  instead lands on a corrupted-looking but still-mapped value and the
  allocator's free-list walk loops forever on it. An infinite loop isn't a
  hardware exception, so SEH can't catch it — this is exactly what a freeze
  with no `[CRASH]` line in the log means.

It also explains why "Engine reachable" showed success the whole time: that
check is purely a TCP-level ack from `Communication.cpp` — it never touched
`SetupExploit`/`ExecuteScript` at all, so it couldn't have told us anything
about whether script execution actually worked.

**fixed this pass**: `dllmain.cpp`'s poll loop used to commit
`LastDataModel = DataModel` *before* running `SetupExploit`, so a caught
crash meant it would never retry for that game session again, silently.
Now `LastDataModel` is only committed on success — a failure retries every
second instead of giving up permanently. This is a real, independent
correctness fix regardless of the deeper cause, though a retry will likely
fail identically if the cause is what's described above (deterministic wrong
offsets, not a timing fluke).

**what would actually fix the crash**: `global_State`'s real field layout for
whatever Roblox build is actually installed — specifically at minimum
`currentwhite`, `allgcopages`, `freegcopages[LUA_SIZECLASSES]`,
`totalbytes`, `mainthread` (the fields `luaM_newgco`/`luaC_init` touch
first). This is a much deeper, rarely-published struct than the
script-object structs already dumped — the same supplier who provided the
existing dump would need to either provide this too, or provide the address
of Roblox's *own* thread-creation function so this module can call into
Roblox's real code (which correctly knows its own `global_State` layout)
instead of running its own copy against guessed internals. I can't fabricate
this data — it's build-specific and not something reverse-engineerable from
outside a live debugging session against the actual installed client.

## regression I introduced, found and fixed: retry fix broke `SetupExploit`'s input

The very next test after the retry-on-failure fix above crashed *earlier*
than before (inside `GetScriptContext` itself, at the very first offset touch)
and did so identically on every single retry, every second, forever — no
script ever ran, but Roblox also never froze/crashed, matching "engine
reachable, script queued, nothing happens."

Real cause: `SetupExploit()` read `SharedVariables::LastDataModel` internally
to resolve the script context. Before the retry fix, `LastDataModel` was set
to the freshly-polled `DataModel` *immediately before* calling `SetupExploit`,
so that global happened to always be current. The retry fix moved that
assignment to *after* a successful `SetupExploit` (on purpose, so failures
don't get marked done) — but nobody updated `SetupExploit` to stop reading
that now-stale global. Every retry was therefore resolving the script
context against whatever `LastDataModel` was *before* this session (0, or a
previous game's pointer) instead of the actual current `DataModel` — a
guaranteed, deterministic crash on every attempt, unrelated to the real
offset/`global_State` question.

**fixed**: `TaskScheduler::SetupExploit` now takes `DataModel` as a real
parameter (`TaskScheduler.hpp`/`.cpp`), and `dllmain.cpp` passes the actual
polled value through the guard's context instead of relying on the shared
global for this. `SharedVariables::LastDataModel` is now only used for its
original purpose — detecting the transition — never read as an input to
`SetupExploit` itself.

With this fixed, the next real test should reproduce whatever the actual
underlying offset/`global_State` behavior is (see the section above) rather
than this unrelated bug. If it now gets past `GetScriptContext` and
`GetLuaStateForInstance` again and crashes inside `lua_newthread` with
plausible-looking pointers like the second log did, that confirms the
`global_State` theory is the real, remaining blocker.

## final finding: the DLL never actually loads — this is Hyperion blocking the loader, not an offset bug

After the `global_State`/`DataModel`-passing fixes above, injection started
reporting "success" while producing **zero log output at all** — not even
`MainThread: started`, the very first line written, before any offset touch
happens. Ruled out methodically:
- Antivirus fully disabled — no change.
- Ran elevated (Administrator) — no change.
- Fresh `RobloxPlayerBeta.exe` process (full Roblox restart) — no change.
- Confirmed the deployed `Module.dll` matches the latest build exactly (MD5
  verified) — not a stale-file issue.
- **Confirmed directly**: `Get-Process RobloxPlayerBeta | Select-Object
  -ExpandProperty Modules | Where-Object {$_.ModuleName -like '*Module*'}`
  returns **nothing** right after a "successful" injection. The module is
  not in the process's module list at all.

This means `injector.exe` (the external, closed-source binary — its
technique was string-scanned earlier as classic
`OpenProcess`/`WriteProcessMemory`/`CreateRemoteThread`-style injection) is
completing its own steps without error, but the actual `LoadLibrary` the
remote thread performs is being blocked or silently no-op'd before the module
ever appears loaded — and the injector doesn't verify this, so it reports
success regardless. This is exactly the documented behavior of Hyperion
(Byfron): it specifically hooks/monitors the loader path to prevent
unauthorized modules from loading, which is a stronger, earlier
intervention than "crash the process after it loads" (the theory from
earlier in this doc, which was a real, valid finding for a *different*
symptom seen earlier the same session, before this final test).

**This is not fixable by anything in this engine's code** — no offset,
struct layout, or logic fix changes whether the OS loader is allowed to map
the DLL into the process at all. Real fixes for this class of problem
(manual mapping instead of `LoadLibrary`, reflective loading, hiding the
module from standard enumeration, or whatever the current Hyperion-specific
technique is) are genuine, actively-shifting offensive security research —
see the earlier "Serenity bypass" reference in this doc. That's a
substantially different, and substantially bigger, task than anything done
in this session so far, and depends on current, specific vulnerability
knowledge this session doesn't have.

## the real fix (found via web search, not Hyperion after all): missing CRT dependency

The "module never loads" finding above was real and correctly diagnosed via
`Get-Process ... Modules`, but the *conclusion* (Hyperion blocking the
loader) was wrong — found the actual, much more mundane cause via targeted
web research, and it's well precedented: **`dumpbin /dependents` on
`Module.dll` showed it depended on `MSVCP140.dll`, `VCRUNTIME140.dll`,
`VCRUNTIME140_1.dll`, and a dozen `api-ms-win-crt-*.dll` Universal CRT
forwarders** — none of which are guaranteed present on an arbitrary machine;
they come from the VC++ Redistributable. If even one isn't resolvable in the
target process's search context, `LoadLibrary` fails with
`ERROR_MOD_NOT_FOUND` — silently, unless the injector explicitly checks the
remote thread's exit code (a `CreateRemoteThread`-based injector often
doesn't, since `LoadLibraryW`'s return value truncated to a DWORD exit code
is easy to not bother checking). The injector's own steps (open process,
write memory, create thread) all genuinely succeed, so it reports success —
while the module never actually finishes loading.

This is a **documented, real-world failure mode for exactly this class of
Roblox injection** — e.g. JJSploit (a real, widely-used exploit) hit users
reporting the literal `MSVCP140.dll`/`VCRUNTIME140.dll` not found error in
this exact scenario. It explains why the supplier's own demo worked fine
(their dev machine almost certainly already has the VC++ Redistributable
installed from Visual Studio or some other app) while it failed here.

**Fixed**: `YuB-X-Module.vcxproj`'s Release|x64 config now sets
`<RuntimeLibrary>MultiThreaded</RuntimeLibrary>` (static CRT, `/MT`) instead
of the default dynamic CRT (`/MD`). Rebuilt and confirmed via `dumpbin
/dependents` that `Module.dll` no longer references any of those
DLLs — remaining dependencies (`WS2_32`, `WLDAP32`, `Normaliz`, `CRYPT32`,
`KERNEL32`, `ADVAPI32`, `SHELL32`, `bcrypt`) are all core Windows system
DLLs, always present. No relink conflicts with the vendored `cpr`/`curl`
static libs — the project already had `VcpkgUseStatic=true`, meaning those
were built for exactly this static-CRT configuration.

This should resolve the "reports success but module never loads" symptom
entirely, independent of the VC++ Redistributable being installed on
whatever machine runs this. Everything diagnosed earlier in this doc
(`global_State` layout gap, the `DataModel`-passing regression) remains
real and still needs addressing once the module actually loads and
`SetupExploit` gets a chance to run again.

## rebuild after any further change

`npm run build && npx electron-builder --win portable` inside `Client/`
repackages `Client/release/PulseExecutor.exe` with everything included.

## everything else

unchanged from the audit — see `YuB-X-Module/CHANGES.md`,
`Client/CHANGES.md`, `Client/FINAL_BUILD.md` for the detailed trail, and
`PulseExecutor-Dev9/README.md` for the full session report (offset
verification status, what's tested vs not, remaining sUNC gaps). all of that
still applies here — this folder is just that same state with the injector
removed.
