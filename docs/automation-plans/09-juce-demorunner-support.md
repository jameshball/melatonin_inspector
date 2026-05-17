# 09. JUCE DemoRunner Support

## Goal

Add direct support for testing against JUCE DemoRunner so automation coverage is
validated against a broad, real JUCE application instead of only a custom
fixture.

This work has two milestones:

- Initial wrapper: build and launch an instrumented DemoRunner, take a
  snapshot and screenshot, and prove one CLI and one MCP call.
- Full deterministic suite: run broader navigation and control scenarios once
  runtime and flake rate are measured.

Current implementation status:

- The wrapper builds a patched DemoRunner and advertises `juce_demorunner`.
- The C++ harness has been expanded from initial wrapper coverage to a
  deterministic real-app E2E pass.
- The E2E target should be named `melatonin-demorunner-e2e`; old `smoke`
  wording is obsolete and should not be used for new CI or documentation.

## Playwright Equivalent

This is equivalent to running a real app E2E suite rather than unit-testing
isolated components.

## JUCE-Specific Differences

- DemoRunner lives inside the fetched JUCE source tree.
- It is not part of this repo and should not be forked into tracked source.
- It includes demos that require hardware, network, web views, cameras, video,
  or heavyweight renderers. Hardware/network/web/video demos should be skipped
  for deterministic PR gating, but OpenGL heavyweight rendering is now a
  required screenshot probe because it validates component-source framebuffer
  compositing.
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

Early wrapper scenario:

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
- Select additional GUI demos: CodeEditorDemo, ComponentDemo,
  ComponentTransformsDemo, DialogsDemo, GridDemo, ImagesDemo, FontsDemo.
- Select one deterministic Audio demo: AudioSettingsDemo.
- Select one deterministic DSP demo: GainDemo.
- Select deterministic Utilities demos: ValueTreesDemo and XMLandJSONDemo.
- Each selected demo must perform at least one meaningful interaction on the
  demo surface itself, not only open the file and switch to the Code tab.
  Semantic controls should assert their new values; visual-only demos should
  capture before/after evidence and assert the pixels changed.
- DialogsDemo must open a non-native `AlertWindow`, prove it appears in
  `windows`, capture it with component screenshots, fill its text editor,
  select its combo box, click the primary button, and dismiss the follow-up
  result dialog through window-local coordinate input.
- Select OpenGLAppDemo, OpenGLDemo, and OpenGLDemo2D, then capture component
  source evidence that includes OpenGL pixels plus JUCE overlays and controls.
- Switch Demo, Code, and Settings tabs.
- Scroll Settings viewport.
- Select a LookAndFeel ComboBox option.
- Return to home.

Skip or quarantine:

- CameraDemo.
- VideoDemo.
- WebBrowserDemo.
- Hardware or network dependent demos.
- Anything that requires audio devices beyond default CI availability.

OpenGL rule:

- OpenGLAppDemo, OpenGLDemo, and OpenGLDemo2D are not quarantined.
- The test must wait for each demo to render, capture `source=component`
  evidence, and assert the scene has meaningful pixel variation.
- If OpenGL component-source capture fails under CI, the screenshot endpoint or
  CI windowing setup must be fixed rather than silently skipping the demo.

## Test Matrix

C++ DemoRunner E2E self-test:

- launches instrumented DemoRunner.
- waits for `juce_demorunner` session.
- lists session through CLI.
- captures root screenshot.
- snapshots root.
- navigates side panel.
- selects deterministic demos across GUI, Audio, DSP, and Utilities categories.
- interacts with each selected deterministic demo using real exposed controls,
  pointer input, keyboard input, drag/scroll, or layout resizing as appropriate.
- selects OpenGLAppDemo/OpenGLDemo/OpenGLDemo2D and captures component-source
  OpenGL screenshot evidence.
- performs semantic actions from earlier workstreams.
- runs representative MCP snapshot and screenshot calls.
- exits DemoRunner cleanly.

CI:

- Deterministic E2E is PR-gated under `xvfb-run` once it stays on the safe
  allowlist.
- Linux CI should force software OpenGL with `LIBGL_ALWAYS_SOFTWARE=1` for the
  DemoRunner E2E to reduce GPU-driver variance.
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
- `demorunner_opengl_screenshot_blank`
- `demorunner_opengl_unavailable`

## Acceptance Criteria

- DemoRunner automation builds through CMake.
- DemoRunner E2E runs in PR CI.
- The suite validates navigation across real pages/tabs/panels.
- The suite does not rely on hardware, network, web, or video demos.
- The suite does include OpenGLAppDemo, OpenGLDemo, and OpenGLDemo2D and proves
  component-source screenshot capture can see heavyweight rendered pixels.

## Evidence Artifacts

- DemoRunner screenshots for startup, side panel, selected demos, Code tab,
  Settings tab, OpenGL root/component compositing, and OpenGL clipped scenes.
- DemoRunner trace once tracing exists.
- CI logs include selected category/demo names.
