# Melatonin Inspector Automation Protocol

`melatonin_inspector` includes an opt-in automation endpoint for local JUCE UI
inspection and control. The interface is intentionally similar to Playwright:
take a compact snapshot, locate an element, act on it, wait for the UI to
settle, and repeat. It is exposed in two user-facing forms:

- `melatonin-ui`, a native CLI.
- `melatonin-ui mcp`, a native MCP stdio server for LLM clients.

The implementation is C++ only. No Node or JavaScript runtime is required.

## Status

This document describes the current interface. The live MCP schema is still the
authority for exact tool shapes because MCP clients consume `tools/list`
directly.

Known current limitations:

- `snapshot --since` reports that a snapshot changed and returns current
  context; it is not yet a true structural diff of added, removed, and updated
  nodes.
- Refs are short-lived snapshot refs, not persistent component identities.
- If multiple sessions use the same `sessionName`, CLI/MCP selection currently
  chooses the newest reachable matching advertisement.
- The endpoint binds to `127.0.0.1` and is intended for trusted local
  development, not production or remote control.

## Enabling Automation

Build the target with automation enabled:

```cmake
target_compile_definitions(YourProject PRIVATE MELATONIN_INSPECTOR_ENABLE_AUTOMATION=1)
```

Enable automation on an inspector instance:

```cpp
melatonin::Inspector inspector { *this };

melatonin::AutomationOptions options;
options.sessionName = "MyPlugin";
inspector.enableAutomation (options);
```

`AutomationOptions`:

| Field | Default | Meaning |
| --- | --- | --- |
| `sessionName` | JUCE application name, then `melatonin` | Public name used by CLI and MCP session selection. |
| `authToken` | Random UUID | Token stored in the local advertisement file and required by the endpoint. |
| `port` | `0` | Local TCP port. `0` lets the OS choose a free port. |
| `advertise` | `true` | Write a session advertisement file so CLI/MCP can discover the app. |
| `allowInput` | `true` | Allow user-like input and semantic control actions. |
| `allowMutation` | `true` | Allow inspector mutations such as bounds/properties. |
| `allowFileWrite` | `false` | Allow endpoint-side screenshot and trace files. |
| `artifactRoot` | empty | Optional directory that file output must stay inside. |

File output is disabled by default. Enable it explicitly:

```cpp
options.allowFileWrite = true;
options.artifactRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("my-plugin-automation");
```

## Session Discovery

Each automation instance writes one JSON advertisement file under:

```text
$TMPDIR/melatonin_inspector/sessions/
```

On Windows this uses `%TEMP%` instead of `$TMPDIR`. The session directory is
created with private permissions where the platform supports that.

Advertisement files are named:

```text
<pid>-<sanitized-session-name>.json
```

Each file contains:

```json
{
  "pid": 12345,
  "session": "MyPlugin",
  "root": "MainComponent",
  "host": "127.0.0.1",
  "port": 54321,
  "token": "...",
  "createdAt": "2026-05-17T12:00:00Z"
}
```

`melatonin-ui list` loads all advertisement files and only shows sessions that
still accept a socket connection.

## Multiple Instances

Multiple instances of the same app can run at the same time. They each get a
separate advertisement file because the filename includes the process id.

Current selection behavior:

- If exactly one reachable session exists, CLI commands can omit `-s`.
- If `-s <name>` matches multiple sessions with the same `sessionName`, the CLI
  and MCP server choose the most recently modified matching advertisement.
- `-s <pid>` explicitly targets the session with that advertised process id.
- `-s` also matches substrings of the advertisement file path, so a
  `pid-session` fragment can target a specific instance.

Example:

```sh
melatonin-ui list
# MyPlugin pid=12345 root="MainComponent" port=54321
# MyPlugin pid=67890 root="MainComponent" port=54322

melatonin-ui -s 12345 snapshot
melatonin-ui -s 67890 click --role button --name Apply
```

Recommended approach for reliable multi-instance work:

```cpp
options.sessionName = "MyPlugin-" + juce::String (juce::Time::currentTimeMillis());
```

or use a domain-specific identifier such as `MyPlugin-main`, `MyPlugin-auhost`,
or `MyPlugin-vst3host`.

## CLI Overview

```sh
melatonin-ui list
melatonin-ui mcp
melatonin-ui -s <session> capabilities
melatonin-ui -s <session> snapshot
melatonin-ui -s <session> describe <ref>
melatonin-ui -s <session> click --role button --name Apply
```

CLI snapshot output defaults to compact text. Use `--json` or
`--format json` for JSON.

Common locator flags:

```text
--role <role>
--name <text>
--text <text>
--component-id <id>
--component-name <name>
--test-id <id>
--class <class-substring>
--value <text>
--has-text <text>
--nth <index>
--exact
--visible | --hidden
--enabled | --disabled
--focused
--selected | --not-selected
```

Common action flags:

```text
--timeout-ms <ms>
--timeout <ms>
--force
--trial
```

`--trial` runs locator resolution and actionability checks without performing
the action. `--force` bypasses actionability checks where the endpoint supports
that.

## MCP Overview

Run the native MCP server:

```sh
melatonin-ui mcp
```

The MCP server discovers the same local automation sessions as the CLI. Every
tool that targets an app takes a `session` argument. `juce_snapshot` defaults to
compact JSON, which is the best default for LLM clients.

Top-level MCP tools:

| Tool | Purpose |
| --- | --- |
| `juce_list_sessions` | List reachable automation sessions. |
| `juce_capabilities` | Return protocol, feature, and security state. |
| `juce_windows` | List automation-owned windows/dialogs/popups. |
| `juce_snapshot` | Return a compact or full component snapshot. |
| `juce_describe` | Inspect one component plus local context. |
| `juce_locator` | Return matching nodes for a locator. |
| `juce_count` | Count locator matches without dumping nodes. |
| `juce_screenshot` | Return PNG image content and/or write a file. |
| `juce_trace_start` | Start recording structured action trace events. |
| `juce_trace_stop` | Stop tracing and write the trace file. |

Action MCP tools:

```text
juce_click
juce_dblclick
juce_right_click
juce_click_xy
juce_hover
juce_mouse_move
juce_mouse_down
juce_mouse_up
juce_wheel
juce_drag_xy
juce_drag
juce_drag_to
juce_type
juce_fill
juce_clear
juce_press
juce_key_down
juce_key_up
juce_check
juce_uncheck
juce_set_checked
juce_set_value
juce_select_option
juce_select_tab
juce_set_bounds
juce_set_property
```

Wait MCP tools:

```text
juce_wait
juce_wait_for_ref
juce_wait_for_locator
juce_wait_for_text
juce_wait_for_value
juce_wait_for_snapshot_change
```

Use MCP `tools/list` for the exact JSON schemas. Target-based action tools
advertise `timeoutMs`, `force`, and `trial` when supported.

## Endpoint Protocol

The CLI and MCP server talk to a local newline-delimited JSON protocol over a
`127.0.0.1` `StreamingSocket`.

Request:

```json
{
  "id": "1",
  "token": "advertised-auth-token",
  "method": "snapshot",
  "params": {
    "mode": "interesting",
    "format": "json"
  }
}
```

Success response:

```json
{
  "id": "1",
  "ok": true,
  "result": {}
}
```

Error response:

```json
{
  "id": "1",
  "ok": false,
  "error": {
    "code": "locator_not_found",
    "message": "Locator did not match any component.",
    "suggestedNextCommand": "snapshot"
  }
}
```

MCP endpoint errors are returned as MCP tool results with `isError: true`.
They are not JSON-RPC protocol errors.

## Capabilities

```sh
melatonin-ui -s MyPlugin capabilities
```

Current feature flags include:

```json
{
  "features": {
    "locators": true,
    "actionability": true,
    "semanticControls": true,
    "richInput": true,
    "screenshots": true,
    "nativeScreenshots": true,
    "tracing": true,
    "windows": true,
    "tokenEfficientSnapshots": true,
    "snapshotModes": ["interesting", "full", "minimal"],
    "scopedSnapshots": true,
    "describe": true,
    "count": true
  },
  "security": {
    "allowInput": true,
    "allowMutation": true,
    "allowFileWrite": false,
    "artifactRoot": ""
  }
}
```

## Locators

Locators are Playwright-style strict target selectors. Actions that accept a
target can take either `ref` or `locator`, but not both.

MCP locator shape:

```json
{
  "role": "button",
  "name": "Apply",
  "text": "Apply",
  "componentId": "main.apply",
  "componentName": "applyButton",
  "testId": "main.apply",
  "class": "TextButton",
  "value": "0.5",
  "hasText": "Gain",
  "exact": true,
  "visible": true,
  "enabled": true,
  "focused": false,
  "selected": true,
  "nth": 0
}
```

Matching behavior:

- `componentId` and `testId` both match `Component::getComponentID()`.
- `componentName` matches `Component::getName()`.
- Text fields normalize whitespace and are case-insensitive.
- `exact: true` switches string matching from contains to equality.
- Locator actions are strict: zero matches fail; multiple matches fail unless
  `nth` narrows the match.
- Actions default to visible targets. Query operations can inspect hidden
  targets when requested.

Examples:

```sh
melatonin-ui -s MyPlugin locator --role button --name Apply
melatonin-ui -s MyPlugin count --role slider
melatonin-ui -s MyPlugin click --test-id main.apply
melatonin-ui -s MyPlugin fill --component-name username "james"
```

## Refs

Snapshots assign refs like `m1-4`. A ref points to a component in the current
snapshot generation.

Important rules:

- Refs are not stable across snapshots.
- Take a fresh snapshot before using refs.
- Prefer locators for robust scripts and LLM workflows.
- `stale_ref` means the ref is unknown to the current endpoint state; run a new
  snapshot or use a locator.

## Snapshots

CLI:

```sh
melatonin-ui -s MyPlugin snapshot
melatonin-ui -s MyPlugin snapshot --json
melatonin-ui -s MyPlugin snapshot --full --format json
melatonin-ui -s MyPlugin snapshot --ref m1-8 --depth 2
melatonin-ui -s MyPlugin snapshot --role dialogWindow --json
```

MCP:

```json
{
  "session": "MyPlugin",
  "mode": "interesting",
  "format": "json",
  "depth": 8
}
```

Snapshot modes:

| Mode | Meaning |
| --- | --- |
| `interesting` | Default compact, action-oriented tree. Keeps windows, dialogs, semantic containers, actionable controls, values, state, and context ancestors. |
| `full` | Complete serialized component tree. Use when debugging serializer behavior or missing compact context. |
| `minimal` | Smaller compact tree. Bounds are omitted by default unless requested. |

Snapshot options:

| Option | CLI | MCP |
| --- | --- | --- |
| mode | `--full`, `--interesting`, `--minimal`, `--mode` | `mode` |
| format | `--format text|json`, `--json` | `format` |
| depth | `--depth n` | `depth` |
| scope by ref | `--ref m1-4` | `ref` |
| scope by window | `--target root|window-1` | `target` |
| scope by locator | locator flags | `locator` |
| include hidden | `--include-hidden` | `includeHidden` |
| include disabled | default true, `--exclude-disabled` | `includeDisabled` |
| actions | default true, `--no-actions` | `includeActions` |
| bounds | default true, `--no-bounds` | `includeBounds` |
| node cap | `--max-nodes n` | `maxNodes` |
| child cap | `--max-children n` | `maxChildrenPerContainer` |
| text cap | `--max-text n` | `maxTextLength` |
| changed context | `--since <stateHash>` | `since` |

Snapshot JSON includes:

```json
{
  "generation": 1,
  "mode": "interesting",
  "stateHash": "...",
  "text": "Window ...",
  "tree": {}
}
```

Interesting snapshots may include `actions`, `omittedChildren`, and
`omittedOptions` when output is capped.

## Describe

`describe` is the second-tier inspection tool for LLMs. Use it after a compact
snapshot when one component needs more local context.

```sh
melatonin-ui -s MyPlugin describe m1-8 --depth 2 --json
melatonin-ui -s MyPlugin describe --role slider --name Gain --json
```

It returns:

- matched component summary
- ancestor path
- immediate interesting children
- state/value/bounds
- action hints
- compact text

## Count

`count` returns only the number of matching components:

```sh
melatonin-ui -s MyPlugin count --role button
```

Use this to check uniqueness before acting without spending tokens on a tree.

## Actionability And Auto-Wait

Locator-based actions retry until they resolve and pass actionability, or until
`timeoutMs` expires. The default endpoint timeout is 5000 ms.

Actionability checks include:

- target is attached
- target is showing
- target is enabled
- target has non-empty bounds
- target receives pointer events at its center

`force` bypasses actionability checks. `trial` reports actionability without
performing the action.

```sh
melatonin-ui -s MyPlugin click --role button --name Apply --timeout-ms 5000
melatonin-ui -s MyPlugin click --role button --name Apply --trial
melatonin-ui -s MyPlugin click --role button --name Apply --force
```

## Actions

Target-based actions accept either a ref or locator options.

Pointer actions:

```sh
melatonin-ui -s MyPlugin click m1-4
melatonin-ui -s MyPlugin click --role button --name Apply
melatonin-ui -s MyPlugin click m1-4 --button right --click-count 1
melatonin-ui -s MyPlugin click m1-4 --position 12,8
melatonin-ui -s MyPlugin dblclick m1-4
melatonin-ui -s MyPlugin right-click m1-4
```

Coordinate actions use window-local coordinates:

```sh
melatonin-ui -s MyPlugin click-xy 100 200 --target root
melatonin-ui -s MyPlugin hover 100 200
melatonin-ui -s MyPlugin mouse-move 100 200
melatonin-ui -s MyPlugin mouse-down 100 200
melatonin-ui -s MyPlugin mouse-up 100 200
melatonin-ui -s MyPlugin wheel 100 200 --dy -0.5
melatonin-ui -s MyPlugin drag-xy 100 200 180 260 --steps 8
```

Keyboard and text:

```sh
melatonin-ui -s MyPlugin type m1-8 "hello"
melatonin-ui -s MyPlugin fill --component-name username "james"
melatonin-ui -s MyPlugin clear m1-8
melatonin-ui -s MyPlugin press Return --ref m1-8
melatonin-ui -s MyPlugin key-down Control+K
melatonin-ui -s MyPlugin key-up Control+K
```

Semantic controls:

```sh
melatonin-ui -s MyPlugin check --role toggleButton --name Enabled
melatonin-ui -s MyPlugin uncheck --role toggleButton --name Enabled
melatonin-ui -s MyPlugin set-checked m1-5 true
melatonin-ui -s MyPlugin set-value --role slider --name Gain 0.75
melatonin-ui -s MyPlugin select-option --role comboBox --name Mode --text "Advanced"
melatonin-ui -s MyPlugin select-tab --component-name main.tabs --name Settings
```

Dragging:

```sh
melatonin-ui -s MyPlugin drag m1-4 --dx 30 --dy 0 --steps 6
melatonin-ui -s MyPlugin drag-to m1-4 m1-9 --steps 10
melatonin-ui -s MyPlugin drag-to --component-name source --target-component-name target
```

Mutation tools:

```sh
melatonin-ui -s MyPlugin set-bounds m1-4 --x 20 --y 40 --w 200 --h 48
melatonin-ui -s MyPlugin set-property m1-4 alpha 0.5
melatonin-ui -s MyPlugin set-property m1-4 visible false
```

Mutation tools require `allowMutation=true`.

## Waits

```sh
melatonin-ui -s MyPlugin wait --ms 250
melatonin-ui -s MyPlugin wait-for-ref m1-4 --timeout-ms 5000
melatonin-ui -s MyPlugin wait-for-locator --role button --name Apply --timeout-ms 5000
melatonin-ui -s MyPlugin wait-for-text "Ready" --timeout-ms 5000
melatonin-ui -s MyPlugin wait-for-value --role slider --name Gain --value 0.75 --timeout-ms 5000
melatonin-ui -s MyPlugin wait-for-snapshot-change --state-hash <stateHash> --timeout-ms 5000
```

MCP equivalents are named `juce_wait`, `juce_wait_for_ref`,
`juce_wait_for_locator`, `juce_wait_for_text`, `juce_wait_for_value`, and
`juce_wait_for_snapshot_change`.

## Screenshots

```sh
melatonin-ui -s MyPlugin screenshot --target root --file startup.png --no-base64
melatonin-ui -s MyPlugin screenshot --ref m1-4 --file control.png
melatonin-ui -s MyPlugin screenshot --role slider --name Gain --file gain.png
melatonin-ui -s MyPlugin screenshot --target root --clip-x 0 --clip-y 0 --clip-w 400 --clip-h 300
melatonin-ui -s MyPlugin screenshot --target root --source component
melatonin-ui -s MyPlugin screenshot --target root --source native
```

Options:

| Option | Meaning |
| --- | --- |
| `target` | `root` or a window id from `windows`. |
| `ref` / `locator` | Capture a component instead of a whole window. |
| `source` | `component`, `native`, or `auto`. CLI default is `component`. |
| `file` | Output path. Requires `allowFileWrite=true`. |
| `clipX`, `clipY`, `clipW`, `clipH` | Component-local clip rectangle. |
| `scale` | Output scale, greater than 0 and no more than 4. |
| `includeBase64` | Include encoded PNG bytes. MCP defaults this to true. |

CLI screenshot base64 is off by default; use `--base64` to include it.
MCP screenshot returns image content by default.

Component screenshots composite attached JUCE OpenGL component framebuffers into
the normal JUCE component snapshot where possible. Native screenshots are still
available when the host platform supports `juce::createSnapshotOfNativeWindow`.

## Windows, Dialogs, And Popups

```sh
melatonin-ui -s MyPlugin windows
melatonin-ui -s MyPlugin snapshot --target window-2 --json
melatonin-ui -s MyPlugin screenshot --target window-2 --file dialog.png
```

The `windows` command lists automation-owned top-level components, including
dialogs and non-native JUCE windows where they are discoverable from the JUCE
desktop. Each has an id such as `root` or `window-2`.

`drag_to` currently requires source and target to be in the same automation
window.

## Tracing And Evidence

```sh
melatonin-ui -s MyPlugin trace-start --file trace.json
melatonin-ui -s MyPlugin click --role button --name Apply
melatonin-ui -s MyPlugin trace-stop
```

Trace files contain structured events with:

- timestamp
- method
- summarized params
- elapsed time
- result summary or error summary

If `trace-start --file` is omitted, the endpoint uses
`melatonin-automation-trace.json`. Trace files are written on `trace-stop`.

Screenshots and traces are constrained to `artifactRoot` when it is set.

## Errors

Common error codes:

| Code | Meaning |
| --- | --- |
| `auth_failed` | Request token did not match the advertised session token. |
| `unknown_method` | Endpoint method is not implemented. |
| `no_root` | No root component is attached. |
| `invalid_locator` | Locator/ref arguments are missing or conflicting. |
| `locator_not_found` | No locator match before timeout. |
| `strict_mode_violation` | Locator matched multiple components for a strict action. |
| `stale_ref` | Ref is no longer valid. |
| `operation_timeout` | Auto-wait or wait condition timed out. |
| `target_not_showing` | Target is hidden or offscreen for actionability. |
| `target_disabled` | Target is disabled. |
| `target_empty_bounds` | Target has no usable bounds. |
| `target_not_receiving_events` | Target center is not hit-testable. |
| `input_disabled` | `allowInput=false`. |
| `mutation_disabled` | `allowMutation=false`. |
| `file_write_disabled` | `allowFileWrite=false`. |
| `artifact_path_denied` | File path is outside `artifactRoot`. |
| `screenshot_failed` | Screenshot capture or encoding failed. |

Locator errors may include `matchCount`, `matches`, nearby context, and a
`suggestedNextCommand`.

## Suggested LLM Control Loop

Use a compact-first workflow:

1. `juce_list_sessions`
2. `juce_snapshot` with default `mode=interesting`
3. `juce_count` if target uniqueness is unclear
4. `juce_describe` for one target or panel
5. Perform one action
6. `juce_wait_for_snapshot_change` or a targeted wait
7. `juce_snapshot` scoped to the changed ref/window/locator
8. Use `juce_screenshot` when visual context matters

Prefer locators over refs in reusable scripts. Prefer scoped snapshots and
`describe` over `mode=full` when an LLM is navigating an app.

## DemoRunner Automation Target

The top-level CMake project can build an instrumented JUCE DemoRunner:

```sh
cmake -S . -B /tmp/melatonin-inspector-demorunner \
  -DMELATONIN_INSPECTOR_BUILD_DEMORUNNER_AUTOMATION=ON \
  -DMELATONIN_INSPECTOR_BUILD_CLI=ON \
  -DMELATONIN_INSPECTOR_ENABLE_AUTOMATION=ON
cmake --build /tmp/melatonin-inspector-demorunner --target melatonin-demorunner melatonin-ui --parallel 4
```

The generated app advertises as:

```text
juce_demorunner
```

Try:

```sh
melatonin-ui -s juce_demorunner capabilities
melatonin-ui -s juce_demorunner snapshot --json --depth 3
melatonin-ui -s juce_demorunner screenshot --target root --file startup.png --no-base64
```

The DemoRunner E2E harness writes copied evidence to
`MELATONIN_DEMORUNNER_ARTIFACT_DIR` when set, otherwise to a temp directory
named `melatonin-demorunner-e2e`.
