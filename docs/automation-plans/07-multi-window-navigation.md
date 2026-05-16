# 07. Multi-Window Navigation

## Goal

Support full-app navigation beyond a single root component: multiple top-level
windows, modals, dialogs, tabs, side panels, and list-driven pages.

## Playwright Equivalent

The closest Playwright concepts are pages, dialogs, frames, and locator-driven
navigation. JUCE does not have browser URLs, so this project should define
navigation around visible application structure.

## JUCE-Specific Differences

- JUCE apps can have multiple `DocumentWindow` instances and modal components.
- A melatonin inspector instance has one current root. Additional roots/windows
  should be explicitly registered with the automation controller, or restricted
  to top-level components owned by the same opted-in app/root.
- Modal components may intercept input even when the original root is still
  visible.
- Navigation often means selecting a tab, opening a side panel, choosing a row,
  or closing a dialog.

## Public Protocol Changes

Add:

- `windows`
- `activate_window`
- `snapshot_window`
- `dismiss_dialog`

Defer `close_window`. It is destructive and not required for the first
navigation milestone.

Window target:

```json
{
  "window": {
    "index": 0,
    "title": "DemoRunner",
    "componentId": "mainWindow"
  }
}
```

Snapshots and screenshots should accept `window` targets in addition to root,
ref, and locator.

Coordinate input should also accept a window target so `click_xy`, mouse
move/down/up, wheel, and point-to-point drag can operate inside secondary
windows using that window's local coordinate space.

## CLI Changes

Add:

```sh
melatonin-ui -s app windows
melatonin-ui -s app screenshot --target window-1 --file /tmp/window.png
melatonin-ui -s app click-xy 200 180 --target window-1
melatonin-ui -s app activate-window --title DemoRunner
melatonin-ui -s app dismiss-dialog --button OK
```

Navigation remains command-specific:

```sh
melatonin-ui -s app select-tab --name Settings
melatonin-ui -s app select-row --component-name demos.list --row 2
```

## MCP Changes

Add tools:

- `juce_windows`
- `juce_activate_window`
- `juce_dismiss_dialog`

Extend screenshot and coordinate action tools to accept a window target.

## Internal C++ Endpoint Changes

Add:

- registered root/window enumeration.
- modal component detection.
- active window tracking.
- root/window target resolution.
- window metadata serialization.

Metadata:

- title/name.
- class.
- bounds.
- visible/showing.
- focused.
- modal state.
- root ref if included in current snapshot.

Do not mutate a user app's window hierarchy except for explicit actions such as
dismiss. Dismiss/activation are gated by `allowInput`.

## Fixture Additions

Add:

- secondary DocumentWindow.
- modal AlertWindow or custom modal component.
- side panel/page component.
- list-driven navigation page.
- nested tabs that change visible content.

## DemoRunner Scenarios

- Open Browse Demos side panel.
- Navigate GUI category.
- Select a deterministic demo.
- Switch between Demo, Code, and Settings tabs.
- Return to home.
- Open DialogsDemo's non-native `AlertWindow`, interact with its text editor
  and combo box, capture component screenshots, and dismiss the follow-up
  result dialog using window-local `click-xy`.

## Test Matrix

C++ fixture self-test:

- list windows.
- snapshot secondary window.
- screenshot secondary window.
- activate window.
- modal appears and becomes active target.
- dismiss modal by button.
- unrelated desktop windows are not exposed.
- tab navigation changes content.
- list navigation changes page.

CLI coverage:

- `windows`, `activate-window`, `dismiss-dialog`.
- `screenshot --target window-N`.
- `click-xy --target window-N`.

MCP coverage:

- `juce_windows` returns metadata.
- action can target a modal/window.
- coordinate actions can target a modal/window.

## Failure Modes

- `window_not_found`
- `window_not_visible`
- `modal_required`
- `dialog_button_not_found`
- `navigation_failed`
- `ambiguous_window`

## Acceptance Criteria

- Automation can operate an entire app, not only one static page.
- Modals do not confuse root targeting.
- DemoRunner proves real navigation across tabs and panels.
- Window metadata is compact enough for MCP context.

## Evidence Artifacts

- Window list JSON on failure.
- Screenshots before and after modal/dialog navigation.
