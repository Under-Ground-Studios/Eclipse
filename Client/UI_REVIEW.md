# PulseExecutor — UI review

Findings from running the actual renderer (`Client/src/renderer`) in a real
browser engine and measuring it, not from reading the CSS. Every claim below
was reproduced against the live UI; the measured values are quoted inline.

How it was checked: the renderer was served with Vite and `window.pulse`
replaced by a stand-in that fakes the Electron preload bridge, then driven
with a headless Chromium (screenshots, computed-style/contrast maths, focus
walking, forced tab counts). The harness lives outside the repo in
`/home/user/preview/` — nothing here ships with the app.

What this review does **not** cover: real injection against a live Roblox
process, GPU-composited transparency/rounded corners against a real desktop
wallpaper, and Windows-specific window-manager behavior. Those need the
packaged build on Windows.

---

## P0 — functional bugs

### 1. Tab names disappear entirely once ~10 tabs are open

Reproduced: opened 12 tabs at 1024px wide. The tab strip renders **twelve
identical `×` buttons and no text at all.** Every tab is anonymous; the only
way to find a script is to click through them one at a time.

Cause: `.tab` in `theme.css` sets `max-width: 190px` but no `min-width`, and
the flex default `flex-shrink: 1` applies. Measured: `min-width: auto`,
`flex-shrink: 1`. The `.tabs` container is `overflow-x: auto`, but the tabs
shrink to fit before they ever overflow, so the scrollbar never engages —
`scrollWidth (646) === clientWidth (646)` with 12 tabs open.

```css
.tab {
  min-width: 108px;   /* enough for ~8 chars + dirty dot + close */
  flex-shrink: 0;     /* let .tabs actually scroll instead of crushing tabs */
}
```

With `flex-shrink: 0` the existing `overflow-x: auto` starts working and the
existing `scrollbar-width: none` keeps it looking clean. Consider also
scrolling the active tab into view in `switchTab()`, since it can now sit
off-screen:

```ts
tabElements.get(id)?.scrollIntoView({ block: "nearest", inline: "nearest" });
```

### 2. Sidebar chevrons point the wrong way

The `Workspace` / `Auto Execute` chevrons point **right (▸) while the section
is expanded** and rotate to point **down (▾) when collapsed** — backwards from
every file tree convention.

Measured on load: path `M3 2l4 3-4 3` (a right-pointing glyph) with
`transform: none`. After clicking: `.collapsed` is added, applying
`rotate(-90deg)`.

The markup glyph should be the *expanded* state (down), and `.collapsed`
should rotate it to point right:

```html
<svg class="tree-chevron" data-section="workspace" viewBox="0 0 10 10"><path d="M2 3.5 5 6.5 8 3.5" /></svg>
```
```css
.tree-chevron.collapsed { transform: rotate(-90deg); }  /* now correct as-is */
```

That's the same glyph `console.html` already uses for its collapse button, so
this also makes the two windows consistent.

### 3. The white accent preset makes the Run button unreadable

`ACCENT_PRESETS` ends with `"#f2f4f8"` (near-white). Selecting it and
measuring the Run button: white text `rgb(255,255,255)` on
`rgb(242,244,248)` — **1.10:1**, versus the 4.5:1 needed. The button becomes
a blank white slab. Same for `Run selected`.

`applyAccent()` derives `--accent-strong` by lightening toward white, which
for an already-white accent is a no-op, and `.tabbar-run` hardcodes
`color: white`. Two options:

- Drop `#f2f4f8` from the presets (simplest), or
- Compute readable foreground per accent:

```ts
const L = 0.2126*lin(r) + 0.7152*lin(g) + 0.0722*lin(b);   // relative luminance
root.setProperty("--accent-fg", L > 0.45 ? "#16171a" : "#ffffff");
```
then use `color: var(--accent-fg)` in `.tabbar-run` / `.tree-selection-run`
instead of `white`. This also protects the custom hex/color picker, which
today lets a user pick any near-white and hit the same dead end.

### 4. Keyboard focus is trapped in the editor — the whole footer is unreachable

Walking Tab from the titlebar gives:

```
#btn-minimize → #btn-maximize → #btn-close → #tree-search →
#tree-add-workspace → #tree-selection-clear → .tab-close → #tab-add →
.inputarea → .inputarea → .inputarea → …
```

Once focus reaches Monaco it never leaves: pressing Tab **inserts a tab
character** into the script (verified — the buffer became
`"            -- PulseExecutor"`). Everything after the editor in DOM order —
the process-name input, Inject, Console, Clear, Settings — cannot be reached
by keyboard at all.

Monaco's standard fix is to make Tab move focus until the user opts in:

```ts
editor.updateOptions({ tabFocusMode: true });
// or bind the conventional escape hatch:
editor.addCommand(monaco.KeyCode.Escape, () => editor.trigger("", "editor.action.focusNextGroup", null));
```

Most editors ship `Ctrl+M` to toggle tab-trapping; exposing that (and listing
it in the Shortcuts pane) keeps indent-with-Tab for people who want it while
making the app keyboard-navigable.

Also worth noting: `#tree-selection-clear` is in the focus order while the
selection bar is collapsed and invisible (it's hidden with `height: 0;
opacity: 0`, which does not remove it from the tab order). Add
`visibility: hidden` to the closed state, and drop it when open.

### 5. Toasts cover the footer controls they're reporting on

Measured a single toast: it spans y 714–752 in a 768px-tall window and
overlaps `#btn-settings`, `#sb-filename` and `#sb-position`. A second and
third toast stack upward over the editor. Since `Run` fires a toast on every
execution, the status bar is hidden exactly when you're most likely to be
watching it.

`.toast-stack` is `bottom: 16px` inside `#app`, which sits *over* the 38px
footer. Lift it clear:

```css
.toast-stack { bottom: 62px; }   /* 38px footer + 16px gutter + 8px app padding */
```

---

## P1 — contrast

Measured against the real composited backgrounds (WCAG AA needs 4.5:1 for
body text). These are the failures, worst first:

| Element | Ratio | Text |
|---|---|---|
| `.line-numbers` (Monaco) | **2.04:1** | gutter numbers |
| `.palette-esc`, `label` | 2.97:1 | `Esc`, `Font size` |
| `.tree-selection-run`, `.tabbar-run` | 3.28:1 | `Run selected`, `Run` |
| `.palette-trigger` + its `kbd` | 3.41:1 | `Search or run a command`, `Ctrl K` |
| `.mtk9` (comments) | 3.44:1 | `-- PulseExecutor` |
| `.confirm-ok` | 3.68:1 | `Delete` |
| `.tree-empty`, section headers | 3.77:1 | sidebar copy |
| `.inject-status`, `.footer-meta` | 3.77:1 | `Disconnected`, `Ln 1, Col 1` |

Nearly all of it traces to two tokens. Nudging them fixes most rows at once:

```css
--text-secondary: #b6bbc6;  /* was #a7acb8 → ~4.5:1 on --bg-panel */
--text-tertiary:  #8b909c;  /* was #737884 → ~4.5:1, still clearly de-emphasised */
```

The two outliers need their own values:

```css
"editorLineNumber.foreground": "#6a6a74",  /* was #4a4a52 — 2.04:1 is unreadable */
{ token: "comment", foreground: "8a8a94" } /* was 6c6c74 */
```

The three white-on-accent buttons (`.tabbar-run`, `.tree-selection-run`,
`.confirm-ok`) are fixed by the `--accent-fg` change in P0-3 — at 3.28:1 they
are borderline even with the default blue.

## P1 — hit targets

Below the 24×24px minimum (measured):

| Control | Size |
|---|---|
| `.tab-close` | 17×17 |
| `.tree-add`, `.tree-selection-clear` | 18×18 |
| `.modal-close` | 21×21 |
| `.tree-item-action` (pin/delete) | 18×18 |

These are destructive or frequent actions sitting next to each other in a
27px-tall row. Keep the icons the size they are and grow the target:

```css
.tab-close, .tree-add, .tree-item-action, .modal-close {
  position: relative;
}
.tab-close::after, .tree-add::after, .tree-item-action::after, .modal-close::after {
  content: ""; position: absolute; inset: -4px;   /* 26×26 target, same visuals */
}
```

## P1 — accessibility gaps

- `#tree-search`, `#palette-input`, `#accent-hex` have placeholders but no
  accessible name. Add `aria-label` (the placeholder vanishes on input, so it
  can't serve as the label).
- The settings rail is a set of `<button>`s with no `role="tablist"` /
  `aria-selected`, and the panes have no `role="tabpanel"`. Arrow keys don't
  move between them.
- `#settings-overlay` and `#palette-overlay` don't trap focus or restore it on
  close; `#confirm-overlay` correctly sets `role="dialog"` / `aria-modal`, but
  the other two don't.
- `.tree-item` rows are `<div>`s with click handlers — not focusable, no
  `role`, no keyboard activation. The sidebar is mouse-only.

---

## P2 — polish and interaction

**Empty console reads as broken.** With no logs the console window body is
literally `""` — a large blank panel with a header. Since it's a separate
always-on-top window a user may open before injecting, add an empty state:
*"No output yet. Inject, then run a script."*

**No empty state when all tabs are closed.** Closing the last tab immediately
creates `Script 2`, so you can never actually reach zero — but that also means
`Ctrl+W` on a single tab feels like it did nothing (the name changes and the
content resets). Either keep a real empty state with a "New script" call to
action, or make `Ctrl+W` a no-op on the last tab.

**Two different Run affordances.** `Run` (tab bar) runs the active tab;
`Run selected` (sidebar) runs the multi-select. They look and read almost
identically but scope differently, and `Run selected` is the only hint that
multi-select exists — discoverable solely via a `Ctrl-click to select` tooltip
that appears on section-header hover. Consider a checkbox that fades in on
row hover, which is self-explanatory.

**Splash costs 1.5s on every launch.** `dismissSplash` is held to a floor of
1500ms plus a 180ms delay and a 600ms fade — ~2.3s before the app is usable,
even when it booted in 200ms. For a tool people launch to run one script, cut
the floor to ~400ms and let fast boots feel fast.

**`--shadow-soft` on `.toast` and `.modal`.** The comment in `:root` correctly
notes shadows are kept subtle because the window is transparent — but toasts
and modals sit *inside* the opaque shell, where a stronger shadow would help
them separate. Worth a second, heavier token for interior elevation.

**Settings panes are a fixed 380px tall.** Appearance holds one row of
swatches, leaving ~300px of dead space; Shortcuts has nine rows and is tight.
`min-height: 380px` instead lets the modal breathe.

**`process-hint` duplicates the dot.** The footer shows a green dot, the word
`Running`, *and* `Disconnected` — three status indicators in a row, two of
which describe different things (process alive vs. pipe connected). The dot
plus `Inject`/`Disconnect` button state already carries this; dropping
`process-hint` at all widths (it's already hidden under 980px) would reduce
the noise.

**Sidebar width is fixed at 240px** with no drag handle, and long script names
truncate at ~22 characters (`very-long-script-nam…`). A drag-to-resize splitter
is the single most-requested affordance in editor-style layouts.

---

## What already works well

Worth keeping as-is: the token system is coherent and genuinely used (no stray
hardcoded colors outside the accent path); `prefers-reduced-motion` is honored
with a real override; transitions are confined to `transform`/`opacity`; the
custom scrollbars, the sliding tab indicator, and the drag-and-drop overlay
are all nicely judged. The comment explaining why shadows are deliberately
restrained under a transparent window is exactly the kind of reasoning that
should be in a stylesheet.
