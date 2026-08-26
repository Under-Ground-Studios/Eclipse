# Client dev pass — rewired to the real engine (YuB-X-Module), not Engine/Runtime

Sandbox copy of `D:\Projects\Roblox\PulseExecutor\Client` — the original is
untouched. Companion backend change in `PulseExecutor-Dev8\YuB-X-Module`
(adds the response ack this rewire depends on) and
`PulseExecutor-Dev8\Injector` (adds the CLI flags this rewire depends on).

**Verified, not just written**: `tsc --noEmit` passes clean on all three
compilation units (`tsconfig.main.json` — main process + shared,
`tsconfig.json` — renderer, and `preload.ts` checked manually since no
tsconfig in this project currently covers it) — exit code 0 on each, run
against the real `electron`/`@types/node` type definitions via a symlinked
`node_modules`, not assumed.

## Root cause, confirmed by reading the actual code on both sides, not guessed

The existing Client was fully-formed, working code — just wired to a
different, unrelated backend (`Engine/Runtime`'s named-pipe server,
`Protocol.h`'s versioned+CRC32 framing) that isn't the engine this project
has actually been building (`сурсы/YuB-X-Module`). Three concrete,
independently-confirmed mismatches:

1. **Injector CLI.** `injector.ts` called `PulseInjector.exe --dll <path>
   --process <name>`. The real `madium.cpp` parsed no flags at all — bare
   PID or auto-find-by-window-title only, DLL name hardcoded at compile
   time via `#define MODULE_NAME`.
2. **Wire protocol.** `protocol.ts` built a 10-byte header (version + type
   + little-endian length + CRC32) and expected replies over
   `\\.\pipe\PulseExecutor`. `YuB-X-Module`'s `Communication.cpp` listens on
   a raw TCP socket (`127.0.0.1:6969`), reads a 4-byte **big-endian**
   length prefix, and — before this pass's backend change — sent no
   response at all.
3. **DLL filename.** Client expected `PulseRuntime.dll`; the actual build
   output of `YuB-X-Module.vcxproj` is `Module.dll`.

None of this was "messy" in the sense of bad code — it was coherent code
for a backend that isn't the one that runs. Fixing the mismatch, not
rewriting working logic for its own sake, is what this pass does.

## What changed

**`main/protocol.ts`** — rewritten from scratch to match
`Communication.cpp` exactly: `buildRequest` (4-byte BE length + raw UTF-8
script), `ResponseFramer`/`parseResponseText` (4-byte BE length + UTF-8
text, `"OK..."` / `"ERR:..."`). No version byte, no message-type enum, no
checksum — none of those exist in the real protocol.

**`main/pipeClient.ts` deleted, replaced by `main/runtimeClient.ts`.** Not
a rename-in-place: the connection model is fundamentally different.
`YuB-X-Module`'s `TcpServer` is stateless per connection (accept, read one
script, write one response, close) — there's no persistent session the way
the old named-pipe server had. `RuntimeClient` reflects that: every
`sendScript` is its own connect→write→read→close cycle; "connected" means
"the last request got a real reply," not "a socket is open." `connect()`
verifies reachability by actually sending a harmless real script (a Luau
comment) and confirming a genuine `OK` reply — not a simulated handshake.

**`sendCommand`/`sendHeartbeat` removed — not stubbed, removed.**
`Communication.cpp`'s wire protocol understands exactly one message shape:
"here is a script, queue it." There is no command or heartbeat concept at
the protocol level. Keeping those methods around calling into nothing (or
worse, sending "stop" as if it were Lua source, which would just be a
compile error) would be fake functionality — removed from `ipc.ts`
(`PulseBridge`), `preload.ts`, and `main.ts`'s IPC handlers.

**The Stop button removed from the UI** (`index.html`, `theme.css`,
`app.ts`) for the same reason — it called `sendCommand("stop")`, which no
longer exists, and there is no cancel-in-flight mechanism in the engine to
wire it to honestly. A dead button that silently does nothing is worse
than no button.

**`injector.ts`** — `getDllPath()` now points at `Module.dll`. The
existing `--dll`/`--process` argument construction was already correct
against the *intended* interface — it's `madium.cpp` (in
`PulseExecutor-Dev8\Injector`) that was updated to actually parse those
flags, plus print the `RESULT:SUCCESS`/`RESULT:FAILURE` markers
`injector.ts`'s regex was already looking for but never actually received
(confirmed by reading `madium.cpp`: the old code only ever printed
`"injected\n"`, so the client's success detection was unconditionally
false regardless of real outcome).

**`app.ts`** — Run button wording changed from "Executing script..." /
"Script executed" to "Sending script..." / "Queued — ..." to match what
the engine can actually confirm right now (accepted and queued, not
"finished running without error" — see the backend pass's own note on why
that's a separate, larger change).

## What's still open

1. **Full execution-result feedback.** Right now "success" means "the
   engine accepted the script," not "the script ran without a Luau error."
   Getting the latter requires keeping the TCP connection open across the
   queue→execute round-trip on the backend, which is real, separate scope
   — flagged in both this pass and the Dev8 engine's own `CHANGES.md`, not
   silently assumed solved.
2. **Visual/style polish** — not started this pass. The existing UI is
   already reasonably clean (dark theme, Monaco editor, tab system); this
   pass was entirely about making the transport layer real before
   reskinning anything on top of it.
3. **Packaging paths** — `resources/PulseInjector.exe` and
   `resources/Module.dll` need to actually be placed there for a packaged
   build; not verified this pass since it's a build/packaging step, not a
   source change.
