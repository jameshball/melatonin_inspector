# 08. Tracing And Evidence

## Goal

Add Playwright-style trace evidence for automation sessions so UI iteration can
be debugged after the fact.

## Playwright Equivalent

Model the idea after Playwright tracing: action log, before/after snapshots,
screenshots, timings, and failure context.

## JUCE-Specific Differences

- There is no DOM trace viewer. Start with a simple file format that can be
  inspected by humans, tests, and future tooling.
- Screenshots are component snapshots, not browser screenshots.
- Traces should be useful for both CLI/MCP clients and C++ fixture tests.

## Public Protocol Changes

Add:

- `start_tracing`
- `stop_tracing`

Start params:

```json
{
  "screenshots": true,
  "snapshots": true,
  "screenshotMode": "failure-and-boundaries",
  "maxBytes": 52428800,
  "sources": false,
  "outputDirectory": "/tmp/melatonin-artifacts/trace"
}
```

Endpoint-side trace output requires `allowFileWrite=true` and must be contained
inside the configured artifact root.

Stop returns:

```json
{
  "directory": "/tmp/melatonin-trace",
  "trace": "/tmp/melatonin-trace/trace.json",
  "events": "/tmp/melatonin-trace/events.ndjson",
  "screenshots": 4,
  "snapshots": 8
}
```

## CLI Changes

Add:

```sh
melatonin-ui -s app trace-start --screenshots --snapshots --output /tmp/trace
melatonin-ui -s app trace-stop
```

CLI should also support a convenience wrapper later:

```sh
melatonin-ui -s app trace-run --output /tmp/trace -- click --role button --name Save
```

The wrapper is optional for v1.

## MCP Changes

Add tools:

- `juce_start_tracing`
- `juce_stop_tracing`

Descriptions should explain that traces are for debugging and CI evidence.

## Internal C++ Endpoint Changes

Add:

- `TraceRecorder`
- trace session state.
- event writer.
- snapshot writer.
- screenshot writer.
- unique artifact naming.
- max-size enforcement.
- token/path redaction.

Record for each action:

- timestamp.
- method.
- params with auth token redacted.
- resolved target summary.
- before snapshot path.
- after snapshot path.
- screenshot paths if enabled.
- result or error.
- elapsed milliseconds.

Trace snapshots must use a non-mutating serializer or isolated trace generation
so tracing does not invalidate user-visible refs or change action behavior.

File layout:

```text
trace/
  trace.json
  events.ndjson
  snapshots/
    0001-before.json
    0001-after.json
    0001-before.txt
    0001-after.txt
  screenshots/
    0001-before.png
    0001-after.png
```

## Fixture Additions

Add a trace section that:

- starts tracing.
- performs successful actions.
- performs an expected failing action.
- stops tracing.
- validates files exist and contain expected event records.

## DemoRunner Scenarios

Record a trace around:

- open Browse Demos.
- select GUI category.
- select AccessibilityDemo.
- switch Settings tab.
- capture screenshots.

## Test Matrix

C++ fixture self-test:

- start tracing.
- trace successful click.
- trace failed locator/action.
- trace screenshots included.
- trace snapshots included.
- trace max-size limit is enforced.
- auth tokens and denied paths are redacted.
- stop tracing returns paths.
- trace files parse as JSON/NDJSON.

CLI coverage:

- `trace-start`
- `trace-stop`

MCP coverage:

- `juce_start_tracing`
- action while tracing.
- `juce_stop_tracing`

## Failure Modes

- `trace_already_running`
- `trace_not_running`
- `trace_directory_invalid`
- `trace_write_failed`
- `trace_screenshot_failed`
- `trace_size_limit_exceeded`
- `trace_file_write_disabled`

## Acceptance Criteria

- Trace output is deterministic enough for tests.
- Failed actions are recorded before returning an error.
- Token values are never written to trace files.
- Trace output cannot escape the artifact root.
- CI can upload trace directories as artifacts.

## Evidence Artifacts

- Trace directory.
- CI artifact upload for traces on failure.
