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

- JUCE screenshots use `Component::createComponentSnapshot()`.
- Component snapshots can include hidden or offscreen behavior that differs from
  OS-level screenshots. Document this clearly.
- Root/window screenshots are component snapshots, not desktop captures.
- File output is useful for CLI and CI, while MCP should return image content.

## Public Protocol Changes

Extend `screenshot`:

```json
{
  "target": "root",
  "ref": "m1-4",
  "locator": { "role": "button", "name": "Save" },
  "window": { "index": 0 },
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

## CLI Changes

Add:

```sh
melatonin-ui -s app screenshot --target root --file /tmp/root.png
melatonin-ui -s app screenshot --component-name nav.editor --file /tmp/button.png
melatonin-ui -s app screenshot --role button --name Save --file /tmp/save.png
melatonin-ui -s app screenshot --target root --clip 0,0,320,200 --file /tmp/clip.png
melatonin-ui -s app screenshot --window 0 --file /tmp/window.png
```

If no `--file` is supplied, print JSON metadata and base64 only when
`--format json` is requested. Default CLI output should stay human-friendly.

## MCP Changes

Extend `juce_screenshot`:

- accept locator arguments.
- accept clip and scale.
- return MCP image content.
- include optional text content with file path and dimensions.

## Internal C++ Endpoint Changes

Add helpers:

- resolve screenshot target.
- validate clip rect.
- convert root-local clip to component-local clip.
- encode PNG.
- write optional file.
- return image metadata.
- enforce `allowFileWrite` and artifact-root validation.

Rules:

- Locator screenshot uses strict locator resolution.
- Ref screenshot rejects stale refs.
- Clip must intersect the target bounds.
- Scale must be greater than zero and bounded to a safe maximum.

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

## Acceptance Criteria

- Screenshots are deterministic enough for CI validation.
- MCP image content can be passed directly into an LLM context.
- CLI file output remains simple.
- Locator screenshots use the same locator strictness as actions.
- Base64 is opt-in outside MCP to avoid bloated CLI/protocol responses.

## Evidence Artifacts

- Screenshot files saved under `MELATONIN_AUTOMATION_ARTIFACT_DIR`, with
  `MELATONIN_SCREENSHOT_DIR` treated only as a backward-compatible override if
  retained.
- CI uploads screenshots on failure.
