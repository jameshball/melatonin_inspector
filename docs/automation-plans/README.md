# Automation Planning Roadmap

This directory contains the implementation plans for turning the current
melatonin_inspector automation endpoint into a Playwright-style control surface
for JUCE applications.

The plans are intentionally separate from implementation. Before adding code for
any feature, update the matching markdown file so the protocol shape, C++ test
strategy, CLI surface, MCP surface, and evidence requirements are clear.

## Current Baseline

The branch already has a working automation baseline:

- `melatonin::Inspector::enableAutomation()` starts a local automation endpoint
  when `MELATONIN_INSPECTOR_ENABLE_AUTOMATION=1`.
- `melatonin-ui` discovers advertised sessions, snapshots the component tree,
  captures screenshots, and performs basic ref-based actions.
- `melatonin-ui mcp` exposes the same endpoint to MCP clients over stdio.
- `tests/automation_fixture.cpp` is the canonical C++ end-to-end fixture. It
  launches a JUCE app, starts automation, runs `melatonin-ui` via
  `juce::ChildProcess`, validates screenshots, navigates tabs, clicks by ref and
  coordinates, types text, presses keys, drags controls, and mutates component
  bounds/properties.
- CI runs the fixture app under `xvfb-run` on Linux.

## Direction

Keep future automation work C++ first:

- User JUCE apps remain C++ only. They should not embed Node, JavaScript, or a
  Playwright-style helper runtime.
- The CLI remains a native C++ executable.
- MCP remains available through the native `melatonin-ui mcp` stdio mode.
- No Node, JS E2E harness, JS adapter, or JS helper client should be added.

## Playwright Compatibility Goal

The goal is not browser compatibility. The goal is API familiarity:

- Semantic locators instead of brittle coordinates.
- Strict single-target actions by default.
- Auto-waiting and actionability checks.
- Rich mouse and keyboard input.
- Screenshots as visual context and evidence.
- Trace artifacts for debugging autonomous UI iteration.
- MCP snapshots that are compact enough for LLM use.

Where JUCE differs from the browser, this project should use JUCE concepts
directly. Tabs, side panels, modal windows, and list-driven pages are the
navigation primitives; there are no browser URLs or DOM nodes.

## Sequencing

Implement in this order:

0. Protocol, snapshot model, security gates, and shared C++ test harness.
1. Locator engine.
2. Actionability and auto-wait.
3. DemoRunner E2E wrapper, once locator/actionability primitives exist.
4. Semantic JUCE controls.
5. Rich input.
6. Screenshots and visual context.
7. Waits and assertions.
8. Multi-window navigation.
9. Tracing and evidence.
10. Full JUCE DemoRunner suite.
11. CI and test matrix hardening.

The sequencing is important. Later features should build on locator resolution,
actionability checks, and shared C++ test utilities rather than duplicating their
own target lookup, retry, or child-process logic.

The DemoRunner work is intentionally staged. A thin wrapper should land as soon
as it can prove the automation endpoint works against a real JUCE app, then it
should be expanded into a deterministic E2E suite that covers navigation,
semantic controls, screenshots, MCP, and traces. PR gating should stay tied to
the deterministic allowlist rather than attempting to drive hardware, network,
camera, video, or heavyweight demos.

## Shared Implementation Rules

- Add or update the matching plan file before implementing a feature.
- Keep protocol additions versioned and discoverable through a capabilities
  response.
- Keep snapshot refs and semantic state versions separate: refs are scoped to a
  snapshot generation, while state-change waits should use a canonical semantic
  hash.
- Preserve existing ref-based commands for low-level debugging.
- Add locator support without making old snapshots or refs invalid.
- Return structured error codes suitable for CLI, MCP, and C++ tests.
- Prefer direct JUCE APIs for semantic actions. Use accessibility interfaces as
  a second layer. Fall back to synthesized input only when semantic APIs are not
  available.
- Keep all automation opt-in, localhost-only, and token-protected.
- Gate expanded powers explicitly: user input through `allowInput`, inspector
  mutation through `allowMutation`, and endpoint-side screenshots/traces/file
  output through `allowFileWrite` plus an artifact-root restriction.

## Shared Test Rules

Each feature must include C++ tests at the fixture level and, when exposed, CLI
and MCP coverage:

- Fixture coverage lives in `tests/automation_fixture.cpp` unless the feature
  needs a dedicated fixture.
- CLI coverage should launch `melatonin-ui` with `juce::ChildProcess`.
- MCP coverage should launch `melatonin-ui mcp` with `juce::ChildProcess` and
  exercise `initialize`, `tools/list`, and `tools/call`.
- The MCP harness must require no external runtime, apply client timeouts longer
  than endpoint timeouts, and capture stdout/stderr transcripts on failure.
- DemoRunner coverage should be added once `09-juce-demorunner-support.md` is
  implemented.
- Failure cases are required, not optional.
- Screenshot and trace-producing features must leave inspectable artifacts on
  failure in CI.

## Plan Files

- [00-protocol-and-snapshot-model.md](00-protocol-and-snapshot-model.md)
- [01-locator-engine.md](01-locator-engine.md)
- [02-actionability-and-auto-wait.md](02-actionability-and-auto-wait.md)
- [03-semantic-juce-controls.md](03-semantic-juce-controls.md)
- [04-rich-input.md](04-rich-input.md)
- [05-screenshots-and-visual-context.md](05-screenshots-and-visual-context.md)
- [06-waits-and-assertions.md](06-waits-and-assertions.md)
- [07-multi-window-navigation.md](07-multi-window-navigation.md)
- [08-tracing-and-evidence.md](08-tracing-and-evidence.md)
- [09-juce-demorunner-support.md](09-juce-demorunner-support.md)
- [10-ci-and-test-matrix.md](10-ci-and-test-matrix.md)
