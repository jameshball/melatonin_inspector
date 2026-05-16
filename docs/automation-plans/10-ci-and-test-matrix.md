# 10. CI And Test Matrix

## Goal

Define the complete C++-first test strategy for the automation roadmap,
including endpoint behavior, CLI integration, MCP integration, fixture coverage,
DemoRunner coverage, screenshots, traces, and CI artifacts.

## Playwright Equivalent

This plays the role of Playwright's browser-level E2E confidence, but adapted to
native JUCE applications and C++ fixtures.

## JUCE-Specific Differences

- GUI tests need platform windowing support.
- Linux CI should run under `xvfb-run`.
- macOS and Windows smoke coverage can remain build-focused unless stable GUI
  execution is added later.
- MCP runs through the native C++ `melatonin-ui mcp` stdio mode. CI must not
  require Node.
- Full DemoRunner PR gating should be staged. Start with smoke gating, measure
  runtime/flakiness, then promote the full suite.

## Public Protocol Changes

No direct protocol changes. CI should validate the protocol and MCP tool schemas
through the endpoint capabilities and `tools/list`.

## CLI Changes

No direct CLI-only changes. CI should cover every CLI command introduced by the
feature plans.

## MCP Changes

No direct MCP-only changes. CI should cover every MCP tool introduced by the
feature plans using a C++ child-process JSON-RPC harness.

## Internal C++ Test Infrastructure

Extract shared helpers from `tests/automation_fixture.cpp`. This is now a
prerequisite before adding more feature test sections:

- process runner for CLI.
- process runner for MCP.
- JSON request/response helpers.
- PNG validation helpers.
- snapshot tree search helpers.
- retry helpers.
- screenshot/trace artifact path helpers.
- failure transcript writer.

Potential location:

```text
tests/automation_test_helpers.h
```

Keep helpers header-only unless CMake structure makes a small test support
target cleaner.

MCP harness requirements:

- launch `melatonin-ui mcp`.
- use no external runtime beyond the built C++ executable.
- send `initialize`, `tools/list`, and `tools/call`.
- validate tool schemas and required fields.
- distinguish JSON-RPC errors from endpoint errors returned by a tool.
- capture stdout/stderr and request/response transcripts.
- use client timeouts larger than endpoint timeouts.

## Fixture Additions

`tests/automation_fixture.cpp` remains the fast PR-gated app for endpoint and
CLI behavior.

It should eventually cover:

- session discovery.
- auth failure.
- snapshots.
- locators.
- actionability.
- semantic controls.
- rich input.
- screenshots.
- waits.
- multi-window/modal behavior.
- traces.
- representative MCP calls.

## DemoRunner Scenarios

The DemoRunner suite should cover broad real-world behavior, not every failure
mode. Failure modes belong primarily in the controlled fixture.

DemoRunner coverage:

- startup.
- side panel navigation.
- category/demo selection.
- tab navigation.
- scrollable settings.
- real buttons/toggles/sliders/combos/text where deterministic.
- screenshots.
- representative MCP snapshot/screenshot/action.

## Test Matrix

Fast fixture CI:

- Linux JUCE 8 with automation enabled.
- Build `automation_fixture`.
- Build `melatonin-ui`.
- Run fixture app under `xvfb-run`.
- Fixture self-test launches CLI and MCP as child processes.
- Artifact root is set with `MELATONIN_AUTOMATION_ARTIFACT_DIR`.

Compatibility CI:

- Existing non-automation builds stay in the matrix.
- JUCE 7 build remains a sanity check where supported.
- macOS and Windows continue to compile the module and existing examples.

DemoRunner CI:

- Linux JUCE 8.
- Automation enabled.
- DemoRunner automation option enabled.
- Smoke E2E under `xvfb-run` in PR CI.
- Full deterministic E2E under `xvfb-run` in scheduled/manual CI until promoted.
- Timeout larger than fixture job.

Artifact policy:

- Always upload screenshots/traces from failed automation jobs.
- Optionally upload DemoRunner screenshots on success for early development.
- Keep artifact paths under a single `MELATONIN_AUTOMATION_ARTIFACT_DIR`.
- Add explicit `actions/upload-artifact` steps for fixture and DemoRunner
  automation jobs.

## Failure Modes

- fixture app fails to start.
- CLI not found.
- MCP process not found.
- session advertisement timeout.
- stale session file.
- screenshot file missing.
- trace file missing.
- DemoRunner patch/build failure.
- xvfb/windowing failure.
- flaky timeout.

## Acceptance Criteria

- Every feature plan adds C++ self-test coverage before being considered done.
- Every public CLI addition is covered.
- Every public MCP addition is covered by a C++ JSON-RPC child-process test.
- DemoRunner smoke E2E is PR-gated once implemented.
- Full DemoRunner E2E is promoted to PR gating only after measured stability.
- CI artifacts make failures inspectable without rerunning locally.

## Evidence Artifacts

- CI logs.
- root and targeted screenshots.
- trace directories.
- final JSON snapshot on failure.
- MCP request/response transcript on MCP failures.
