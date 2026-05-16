# 09. JUCE DemoRunner Support

## Goal

Add direct support for testing against JUCE DemoRunner so automation coverage is
validated against a broad, real JUCE application instead of only a custom
fixture.

This work has two milestones:

- Early smoke wrapper: build and launch an instrumented DemoRunner, take a
  snapshot and screenshot, and prove one CLI and one MCP call.
- Full deterministic suite: run broader navigation and control scenarios once
  runtime and flake rate are measured.

## Playwright Equivalent

This is equivalent to running a real app E2E suite rather than unit-testing
isolated components.

## JUCE-Specific Differences

- DemoRunner lives inside the fetched JUCE source tree.
- It is not part of this repo and should not be forked into tracked source.
- It includes demos that require hardware, network, web views, cameras, video,
  or heavyweight renderers. These should be skipped for deterministic PR gating.
- Current top-level CMake fetches JUCE `develop`, so patch anchors can move.
  DemoRunner automation must either pin a tested JUCE tag/commit for this target
  or fail with an actionable patch-anchor error.

## Public Protocol Changes

No user-facing protocol change is required.

Add CMake option:

```cmake
MELATONIN_INSPECTOR_BUILD_DEMORUNNER_AUTOMATION=ON
```

The instrumented app should advertise:

```text
session = juce_demorunner
```

## CLI Changes

No special CLI commands are required. The existing and planned automation CLI
commands should work against the `juce_demorunner` session.

Documentation should include:

```sh
melatonin-ui -s juce_demorunner snapshot
melatonin-ui -s juce_demorunner screenshot --target root --file startup.png
```

File outputs must stay inside the artifact root reported by `capabilities`.

## MCP Changes

No special MCP tools are required. MCP tests should target the
`juce_demorunner` session through existing tools.

## Internal C++ Endpoint Changes

No endpoint changes should be required beyond the feature workstreams that this
suite validates.

## DemoRunner Build Integration

Use a generated patched wrapper:

1. Locate fetched JUCE source.
2. Generate a patched `Source/Main.cpp` into the build directory.
3. Reuse DemoRunner source files from the fetched JUCE tree without editing
   them in place.
4. Add `#include <melatonin_inspector/melatonin_inspector.h>`.
5. Add an Inspector member to the application or main window.
6. After `MainComponent` is installed as content, create the Inspector against
   the content/root component.
7. Call `enableAutomation()` with session `juce_demorunner`.
8. Keep the Inspector hidden unless debugging requires visibility.
9. Fail CMake configuration if patch anchors are not found.

Do not edit the fetched JUCE source tree in place.

Fallback route:

- If patching `Source/Main.cpp` becomes too brittle, try a tiny repo-owned
  launcher that includes DemoRunner UI sources and owns `MainComponent` plus an
  Inspector directly.
- If neither route is viable for the fetched JUCE version, configuration must
  fail with a clear message naming the missing anchor and suggested JUCE version.

## Fixture Additions

Add a separate C++ self-test app or test runner for DemoRunner automation. It
should be built only when `MELATONIN_INSPECTOR_BUILD_DEMORUNNER_AUTOMATION=ON`.

The test runner should reuse shared child-process helpers from the fixture once
those helpers are extracted.

## DemoRunner Scenarios

Early smoke scenario:

- Build instrumented DemoRunner.
- Launch and wait for `juce_demorunner`.
- Capture startup snapshot.
- Capture root screenshot.
- Run one native MCP snapshot call through `melatonin-ui mcp`.
- Quit cleanly.

Full deterministic allowlist:

- Startup snapshot.
- Root screenshot.
- Open Browse Demos.
- Select GUI category.
- Select AccessibilityDemo.
- Interact with buttons/toggles.
- Set or drag a slider.
- Interact with TreeView only if deterministic under CI.
- Switch Demo, Code, and Settings tabs.
- Scroll Settings viewport.
- Select a LookAndFeel ComboBox option.
- Return to home.

Skip or quarantine:

- CameraDemo.
- VideoDemo.
- WebBrowserDemo.
- OpenGL-heavy demos.
- Hardware or network dependent demos.
- Anything that requires audio devices beyond default CI availability.

## Test Matrix

C++ DemoRunner self-test:

- launches instrumented DemoRunner.
- waits for `juce_demorunner` session.
- lists session through CLI.
- captures root screenshot.
- snapshots root.
- navigates side panel.
- selects deterministic demo.
- performs semantic actions from earlier workstreams.
- runs representative MCP snapshot and screenshot calls.
- exits DemoRunner cleanly.

CI:

- Early smoke is PR-gated under `xvfb-run`.
- Full deterministic suite starts as manual/scheduled until runtime and flake
  rate are measured, then becomes PR-gated.
- Longer timeout than the small fixture.
- Upload screenshots/traces on failure.

## Failure Modes

- `demorunner_source_not_found`
- `demorunner_patch_failed`
- `demorunner_patch_anchor_changed`
- `demorunner_build_failed`
- `demorunner_session_timeout`
- `demorunner_navigation_failed`
- `demorunner_quarantined_demo_selected`

## Acceptance Criteria

- DemoRunner automation builds through CMake.
- Early DemoRunner smoke runs in PR CI.
- Full deterministic DemoRunner E2E is promoted to PR CI only after measured
  stability is acceptable.
- The suite validates navigation across real pages/tabs/panels.
- The suite does not rely on hardware, network, or heavyweight rendering demos.

## Evidence Artifacts

- DemoRunner screenshots for startup, side panel, selected demo, Code tab, and
  Settings tab.
- DemoRunner trace once tracing exists.
- CI logs include selected category/demo names.
