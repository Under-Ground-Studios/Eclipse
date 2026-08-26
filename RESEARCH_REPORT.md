# PulseExecutor / YuB-X-Module — итоговый отчёт (25.08.2026)

Метод: чтение реального исходника (`PulseExecutor-Dev10`/`Dev12`), веб-research с упором на 2025–2026, сверка с присланным реальным дампом (`Downloads/message.txt`, билд `ddf602d9cfe44005`). Каждое утверждение помечено: **подтверждено кодом**, **подтверждено внешним источником**, **community-reported**, или **не подтверждено**.

---

## A. Почему реализация показывала ~12% UNC/SUNC?

Две причины, обе подтверждены, ни одна не про "мало функций":

1. **Методология измерения устарела.** Presence-only UNC официально мёртв (репозиторий `unified-naming-convention/NamingStandard` заархивирован владельцем 4 мая 2024 — подтверждено на GitHub). Единственный живой стандарт — sUNC (~8000 строк поведенческих проверок, докс на docs.sunc.su, последнее обновление репозитория 15.06.2026 — подтверждено). Если процент мерился не через актуальный sUNC V2 (который с версии V2 гоняется только внутри официальной test-игры, не в произвольном месте), цифра не отражает реальное состояние модуля.
2. **Historical/до этой сессии**: старый roadmap-документ (`ROADMAP_99_UNC_SUNC.md`) описывал состояние, где были зарегистрированы только `loadstring`/`HttpGet`/`getgenv`/`identifyexecutor` — **это уже не соответствует коду**: `Environment.cpp` регистрирует все 8 библиотек (Closures, Http, Miscellaneous, Metatable, Filesystem, Encoding, Reflection, Debug) — подтверждено прямым чтением файла. Roadmap устарел относительно кода на момент начала этой сессии.

## B. Каких фундаментальных компонентов не хватает?

Только один структурный, подтверждённый самим же дампом: **InstanceRegistry полностью не задампан** (`InstanceUserdataLayout`, `LiveInstanceRegistry`, `ConnectionStructLayout`, `PropertyDescriptorTable` — все `0x0` в `message.txt`, присланном как актуальный подтверждённый дамп). Это блокирует `gethiddenproperty`/`setscriptable`/`getinstances`/`getnilinstances`/полноценный `getconnections` — не потому что код не написан, а потому что смотреть физически некуда. Это единственный настоящий "неизвестный" в базе на сегодня.

Всё остальное, что раньше считалось "недостающим" (Debug, Metatable, Reflection, Encoding, Filesystem, newcclosure/hookfunction) — **уже реализовано и зарегистрировано**, подтверждено построчным чтением.

## C. Что реально даёт современным реализациям высокий процент?

Не breadth (число функций), а прохождение **поведенческих** проверок sUNC — существование функции не засчитывается, должна быть корректная семантика: `hookfunction` должен патчить in-place и позволять вызывать оригинал, `newcclosure` должен быть настоящим C-closure с правильным trampoline, `getgc`/`filtergc` должны безопасно обходить живой GC не аллоцируя во время обхода (иначе краш под нагрузкой), `debug.setupvalue`/`setconstant` обязаны ставить GC write barrier (иначе GC может собрать живой объект). Всё перечисленное **уже сделано правильно в этой базе** — подтверждено чтением `Debug.hpp`, `Reflection.hpp`, `Closures.hpp`.

## D. Какие части требуют глубокого runtime integration?

Closures (newcclosure/hookfunction — прямая работа с `Closure`/`Proto` VM-структурами), Debug (getstack/setstack — работа с `CallInfo`/`lua_State` стеком), Reflection (getgc — обход `luaM_visitgco`), Metatable (hookmetamethod — тот же closure-patch паттерн). Все уже написаны на этом уровне, не Lua-обёртки поверх — подтверждено кодом.

## E. Какие части — обычный API compatibility layer?

Filesystem (`readfile`/`writefile` и т.д. — чистый Win32/std::filesystem, ноль VM-зависимости), Encoding (base64 + реальный вендоренный lz4, не самопальный). Подтверждено кодом — оба файла явно документируют в комментариях, что зависимости от VM/офсетов нет.

## F. Какие части наиболее чувствительны к обновлениям Roblox?

`Roblox/Offsets.hpp` — 7 констант, специфичных для одного билда. **До этой сессии** резолвились напрямую (REBASE), без fallback-механизма → на следующем апдейте либо тихий слом, либо краш. **В этой сессии вживлён и собран** signature-scanner слой (`PatternTable.hpp`/`SignatureScanner.hpp`) с реальными AOB-паттернами (источник: публичный `metixud/Roblox-Dumper`, помечен автором как "discontinued", то есть не проверен именно на дамп-момент вашей версии) — при промахе паттерна тихо падает на старый хардкод, ничего не сломано, но и не панацея: паттерны компилятор-специфичные последовательности байт, переживают апдейты лучше абсолютного адреса, но не гарантированно.

## G. Какие architectural assumptions старых executor-проектов больше не актуальны?

Единственный живой пример (`gladhaxx`, C++, реальная native VM-интеграция) — сам автор прямо пишет: **не работает против Byfron/Hyperion**, до-Hyperion архитектура. Подтверждено: голая инъекция кода без прохождения integrity-handshake Hyperion больше не работает — community consensus 2025-2026, подтверждён многими независимыми источниками (Guided Hacking, robloxexploiting wiki, Endsights). Открытых актуальных (post-Hyperion) референсных архитектур **на GitHub не найдено вообще** — все реальные C++ репозитории мертвы и до-Byfron.

## H. Современная reference-архитектура

Подтверждена структурой вашего же кода (это фактически уже она):

```
Roblox Process (RobloxPlayerBeta.exe, single module — подтверждено
                двумя независимыми публичными дамп-тулами)
│
├── Luau VM (вендоренный форк, структуры подтверждены дампом ddf602d9cfe44005)
│     Proto/Closure/TString/lua_State — часть полей закодирована VMValueN
│     (self-referential XOR/offset transform, подтверждено message.txt)
│
├── Offsets layer
│     hardcoded REBASE() constants ← fallback
│     + Signature scanner (Tier 1.1, вживлён в этой сессии)
│
├── TaskScheduler / Yielding
│     RenderStepped hook → ScriptsHandler → Execution
│     yield/resume через ScriptContextResume + worker thread
│
└── Environment (8 зарегистрированных библиотек)
      Closures, Debug, Reflection, Metatable, Filesystem, Encoding,
      Http, Miscellaneous
```

## I. Какие направления развития дают наибольшую отдачу?

1. Передамп `InstanceRegistry` (единственный структурный блокер, п. B).
2. Проверка метода измерения процента — прогон официального sUNC V2 внутри официальной test-игры, не старого скрипта.
3. Точечная верификация AOB-паттернов из п. F под конкретно ваш билд (паттерны сторонние, не 100%-гарантированные).

## J. Как правильно измерять прогресс?

Только официальный sUNC (docs.sunc.su), только через официальную test-игру (обязательно с V2 — раньше можно было гонять где угодно, теперь нет — подтверждено докс). Замер старым UNC-скриптом или неофициальной копией теста даёт цифру, не соответствующую реальному состоянию модуля — именно это, по всей видимости, и произошло с изначальными "12%".

---

## Верифицировано в этой сессии (сборка + сверка офсетов)

- Полная сверка `Offsets.hpp` / `Encryptions.hpp` / `EncryptionsHelper.hpp` / вендоренных `lobject.h`/`lstate.h` против присланного реального дампа `message.txt` (билд `ddf602d9cfe44005`) — **всё совпадает**, кроме одного неактивного нюанса: `Closure::debugname` в `lobject.h` схлопнут в один закодированный слот вместо двух полей из дампа (обоснованно, т.к. `lapi.cpp`/`lclass.cpp` пишут в него как в `const char*`); ничего в коде сейчас не читает это поле, риска нет.
- Вживлён и собран (MSBuild, exit code 0) signature-scanner слой поверх `Offsets.hpp`.
- Исправлен независимый баг компиляции: `TaskScheduler.cpp`/`Execution.cpp`/`Yielding.cpp`/`Utils.hpp` использовали `Offsets::`/`Roblox::` без `#include <Roblox/Offsets.hpp>` — в боевом конфиге `Release|x64` (`PrecompiledHeader=NotUsing`) это не компилировалось бы вообще.
- Собран полный пакет: `Client/release/PulseExecutor.exe` (Electron-фронтенд + сегодняшний `Module.dll`, хэши совпадают — подтверждено `md5sum`).

Работа велась в `PulseExecutor-Dev12` (песочница-копия `Dev10`), `Dev10` не тронут.
