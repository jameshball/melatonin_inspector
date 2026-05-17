# 05. Screenshots And Visual Context

## Goal

Make screenshots reliable evidence for LLM-driven iteration and CI debugging.
Screenshots should be available for the whole root, a window, a ref, a locator,
or a clipped region.

## Playwright Equivalent

Model the surface after:

- page screenshots.
- locator screenshots.
- clip rect screenshots.
- image payloads in MCP tools.

## JUCE-Specific Differences

- JUCE component/ref screenshots use `Component::createComponentSnapshot()`.
- Screenshots should default to `source=component` for deterministic behavior.
- `source=native` uses JUCE's native-window capture where available.
- Component snapshots can include hidden or offscreen behavior that differs from
  OS-level screenshots. Document this clearly.
- Component screenshots must composite OpenGL-backed components by reading the
  attached OpenGL framebuffer and drawing JUCE component paint/children over it.
  Native-window screenshots remain useful as a fallback/diagnostic source, but
  they are not required for OpenGL visual evidence.
- File output is useful for CLI and CI, while MCP should return image content.

## Public Protocol Changes

Extend `screenshot`:

```json
{
  "target": "root",
  "ref": "m1-4",
  "locator": { "role": "button", "name": "Save" },
  "window": { "index": 0 },
  "source": "component",
  "clip": { "x": 0, "y": 0, "w": 320, "h": 200 },
  "scale": 1.0,
  "includeBase64": false,
  "file": "/tmp/save-button.png"
}
```

Return:

```json
{
  "mimeType": "image/png",
  "width": 320,
  "height": 200,
  "target": { "ref": "m2-4", "name": "Save" },
  "file": "/tmp/save-button.png",
  "base64": "only when includeBase64 is true"
}
```

Response policy:

- Raw protocol includes base64 only when `includeBase64=true`.
- CLI defaults to path and metadata, not base64.
- MCP returns image content because that is the point of the tool.
- Endpoint-side file writes require `allowFileWrite=true` and must stay under
  the configured artifact root.
- `source` accepts `component`, `native`, or `auto`. `component` is deterministic
  JUCE repaint capture plus OpenGL framebuffer compositing. `native` captures
  pixels from the native window and crops back to the target. `auto` may try
  native capture for root screenshots and fall back to component snapshots, but
  the CLI and MCP defaults stay `component`.

## CLI Changes

Add:

```sh
melatonin-ui -s app screenshot --target root --file /tmp/root.png
melatonin-ui -s app screenshot --component-name nav.editor --file /tmp/button.png
melatonin-ui -s app screenshot --role button --name Save --file /tmp/save.png
melatonin-ui -s app screenshot --target root --clip 0,0,320,200 --file /tmp/clip.png
melatonin-ui -s app screenshot --target root --source component --file /tmp/opengl.png
melatonin-ui -s app screenshot --window 0 --file /tmp/window.png
```

If no `--file` is supplied, print JSON metadata and base64 only when
`--format json` is requested. Default CLI output should stay human-friendly.

## MCP Changes

Extend `juce_screenshot`:

- accept locator arguments.
- accept clip and scale.
- accept `source=auto|component|native`.
- return MCP image content.
- include optional text content with file path and dimensions.

## Internal C++ Endpoint Changes

Add helpers:

- resolve screenshot target.
- validate clip rect.
- convert root-local clip to component-local clip.
- crop native-window captures back to the requested root/ref/locator bounds.
- find OpenGL-backed components in the target hierarchy.
- read OpenGL framebuffer pixels on the OpenGL render thread.
- composite those pixels into component screenshots at the component bounds.
- draw JUCE component paint and child controls over the framebuffer read.
- encode PNG.
- write optional file.
- return image metadata.
- enforce `allowFileWrite` and artifact-root validation.

Rules:

- Locator screenshot uses strict locator resolution.
- Ref screenshot rejects stale refs.
- Clip must intersect the target bounds.
- Scale must be greater than zero and bounded to a safe maximum.
- `source=native` fails explicitly if the platform/native handle cannot provide
  a snapshot. `source=auto` may fall back to component snapshots.

## Fixture Additions

Add components with:

- known fixed dimensions.
- nested target bounds.
- partially clipped target.
- locator-only screenshot target.

## DemoRunner Scenarios

- Root screenshot after startup.
- Browse Demos side panel screenshot.
- Demo tab screenshot.
- Code tab screenshot.
- Settings viewport screenshot.
- AccessibilityDemo selected control screenshot.
- OpenGLAppDemo/OpenGLDemo/OpenGLDemo2D component-source screenshots that prove
  GPU-rendered pixels and JUCE overlays/controls are present without native
  screenshots.

## Test Matrix

C++ fixture self-test:

- root PNG signature and dimensions.
- ref PNG dimensions.
- locator screenshot dimensions.
- clip screenshot dimensions.
- invalid clip failure.
- stale ref screenshot failure.
- file output.
- base64 output decodes to PNG signature.
- native-source root screenshot produces a valid PNG.
- file output denied when `allowFileWrite=false`.
- path traversal outside artifact root is denied.

CLI coverage:

- root/ref/locator/clip screenshot commands.

MCP coverage:

- `juce_screenshot` returns `image/png` content.
- metadata text includes dimensions.

## Failure Modes

- `screenshot_target_not_found`
- `invalid_clip`
- `invalid_scale`
- `screenshot_failed`
- `png_encode_failed`
- `file_write_failed`
- `file_write_disabled`
- `artifact_path_denied`
- `invalid_screenshot_source`

## Acceptance Criteria

- Screenshots are deterministic enough for CI validation.
- MCP image content can be passed directly into an LLM context.
- CLI file output remains simple.
- Locator screenshots use the same locator strictness as actions.
- Base64 is opt-in outside MCP to avoid bloated CLI/protocol responses.
- OpenGL-heavy DemoRunner screenshot evidence uses `source=component` and is
  checked for pixel variation rather than only PNG validity.

## Evidence Artifacts

- Screenshot files saved under `MELATONIN_AUTOMATION_ARTIFACT_DIR`, with
  `MELATONIN_SCREENSHOT_DIR` treated only as a backward-compatible override if
  retained.
- CI uploads screenshots on failure.
