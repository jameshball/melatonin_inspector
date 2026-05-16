# 04. Rich Input

## Goal

Expose low-level mouse and keyboard input for cases where semantic control APIs
are unavailable or where user-like interaction is the feature under test.

## Playwright Equivalent

Model the surface after:

- `locator.hover()`
- `locator.click({ button, clickCount, position, modifiers })`
- `page.mouse.move/down/up/wheel`
- `page.keyboard.down/up/press/type`
- `locator.dragTo()`

## JUCE-Specific Differences

- JUCE event synthesis goes through `ComponentPeer::handleMouseEvent()` or
  direct component mouse callbacks depending on target and action.
- Direct component callbacks are useful for fixture-only drag verification, but
  peer-level synthesis better matches real user input.
- Keyboard handling differs between focused components and peer-level key
  events. Text controls should still prefer semantic text APIs for `fill`.

## Public Protocol Changes

Extend `click`:

```json
{
  "button": "left",
  "clickCount": 1,
  "position": { "x": 10, "y": 12 },
  "modifiers": ["shift", "command"]
}
```

Extend `click` rather than adding separate protocol methods for double-click and
right-click. Public protocol methods:

- `hover`
- `mouse_move`
- `mouse_down`
- `mouse_up`
- `mouse_wheel`
- `keyboard_down`
- `keyboard_up`
- `keyboard_press`
- `drag_to`
- `drag_points`

CLI may expose `dblclick` and `right-click` aliases for ergonomics, but they
should serialize to `click` with `clickCount` or `button`.

Coordinate spaces:

- root-local for `click_xy`, `mouse_*`, and `drag_points`.
- target-local for `position` inside locator/ref actions.
- screen coordinates should not be part of v1.

## CLI Changes

Add:

```sh
melatonin-ui -s app hover --component-name hover.target
melatonin-ui -s app click --component-name canvas --position 12,24 --button right
melatonin-ui -s app dblclick --component-name editor.label
melatonin-ui -s app mouse-move 100 120
melatonin-ui -s app mouse-down --button left
melatonin-ui -s app mouse-up --button left
melatonin-ui -s app mouse-wheel --dx 0 --dy -120
melatonin-ui -s app keyboard-press "Command+A"
melatonin-ui -s app drag-to --component-name source --target-component-name target
melatonin-ui -s app drag-points --from 20,20 --to 200,100 --steps 8
```

## MCP Changes

Add tools:

- `juce_hover`
- `juce_mouse`
- `juce_keyboard`
- `juce_drag_to`

`juce_mouse` and `juce_keyboard` are low-level escape hatches. Prefer semantic
tools in descriptions so LLMs do not default to coordinates.

## Internal C++ Endpoint Changes

Add:

- modifier parser.
- mouse button parser.
- key chord parser.
- root/local coordinate conversion helpers.
- peer event synthesis helper for production paths.
- drag sequence helper with configurable steps.

Click behavior:

- For semantic buttons, keep `Button::triggerClick()` as the default.
- For positional or non-button clicks, synthesize peer events.
- Support click count in mouse event construction where JUCE exposes it.
- Direct component callbacks are allowed only for narrow fixture probes where
  peer synthesis cannot expose the state being tested.

Keyboard behavior:

- Targeted key actions grab focus first.
- Global keyboard actions send through root peer.
- Text insertion should remain separate from physical key press behavior.

## Fixture Additions

Add controls/components that record:

- hover entered/exited.
- last mouse position.
- last mouse button.
- click count.
- modifier state.
- wheel delta.
- key down/up order.
- drag start/end/steps.
- drag source and target names.

## DemoRunner Scenarios

- Drag sliders in AccessibilityDemo or WidgetsDemo.
- Scroll Settings viewport with wheel.
- Click tab labels with positions.
- Use keyboard navigation in Code tab or text editor demos where deterministic.

## Test Matrix

C++ fixture self-test:

- hover updates recorded state.
- right click records right button.
- double click records click count.
- positional click hits expected local target.
- mouse move/down/up sequence records coordinates.
- wheel scroll changes viewport position.
- keyboard chord records modifiers.
- drag ref-to-ref reaches target.
- drag points follows configured steps.

CLI coverage:

- representative mouse, keyboard, and drag commands.

MCP coverage:

- `juce_hover`, `juce_mouse`, `juce_keyboard`, and `juce_drag_to` execute.
- `tools/list` schema, required-field validation, unknown-tool behavior, and
  endpoint-error propagation are covered.

## Failure Modes

- `invalid_button`
- `invalid_modifier`
- `invalid_key`
- `invalid_coordinate`
- `target_not_receiving_events`
- `drag_failed`
- `no_root_peer`

## Acceptance Criteria

- Low-level input is available but not required for semantic controls.
- Coordinate spaces are documented and tested.
- Modifier and key naming is stable across CLI and MCP.
- Fixture proves event order, not just final status text.
- DemoRunner proof is added only after peer-level fixture input tests pass.

## Evidence Artifacts

- Snapshot after each input section on failure.
- Optional screenshot for drag failures once tracing exists.
