# 03. Semantic JUCE Controls

## Goal

Add high-level control operations that use JUCE APIs directly instead of relying
only on mouse coordinates. This should let LLMs and scripts operate real JUCE
apps by intent: fill text, check toggles, set sliders, select combo options,
select tabs, scroll, and select rows.

## Playwright Equivalent

Model the intent after:

- `locator.click()`
- `locator.fill()`
- `locator.clear()`
- `locator.check()`
- `locator.uncheck()`
- `locator.setChecked()`
- `locator.selectOption()`
- locator assertions for value/checked state

## JUCE-Specific Differences

- JUCE controls are C++ objects, so direct APIs are usually better than
  synthesized input.
- Some controls expose state through accessibility interfaces rather than public
  widget APIs.
- ComboBox popups are transient JUCE popup windows. Selecting directly through
  `ComboBox::setSelectedId()` is more deterministic than opening the popup.
- ListBox and TableListBox rows may not be stable component children. Prefer
  list APIs for row selection.
- TreeView items are not normal child components. Tree support should be scoped
  carefully and can use accessibility actions when component APIs are not enough.

## Public Protocol Changes

Add v1 methods:

- `fill`
- `clear`
- `check`
- `uncheck`
- `set_checked`
- `set_value`
- `select_option`
- `select_tab`
- `scroll`
- `select_row`

Defer these to phase 2 unless direct, deterministic JUCE support is proven:

- `expand`
- `collapse`
- table cell selection
- accessibility-only tree/list rows

Each method accepts `ref` or `locator` plus method-specific parameters.

Examples:

```json
{ "method": "fill", "params": { "locator": { "role": "editableText", "name": "Search" }, "text": "gain" } }
{ "method": "set_value", "params": { "locator": { "componentName": "controls.slider" }, "value": 75 } }
{ "method": "select_option", "params": { "locator": { "role": "comboBox" }, "label": "Beta" } }
{ "method": "select_tab", "params": { "locator": { "componentName": "fixture.tabs" }, "name": "Advanced" } }
```

Snapshot nodes should expose additional semantic fields where applicable:

- `checked`
- `toggleable`
- `editable`
- `readOnly`
- `selected`
- `selectedIndex`
- `selectedText`
- `options`
- `minimum`
- `maximum`
- `interval`
- `tabNames`
- `currentTab`
- `scrollX`
- `scrollY`
- `viewWidth`
- `viewHeight`

## CLI Changes

Add:

```sh
melatonin-ui -s app fill --component-name editor.text "hello"
melatonin-ui -s app clear --component-name editor.text
melatonin-ui -s app check --component-name controls.power
melatonin-ui -s app uncheck --component-name controls.power
melatonin-ui -s app set-value --component-name controls.slider 75
melatonin-ui -s app select-option --component-name controls.combo --label Beta
melatonin-ui -s app select-tab --component-name fixture.tabs --name Advanced
melatonin-ui -s app scroll --component-name settings.viewport --dy 300
melatonin-ui -s app select-row --component-name demo.list --row 4
```

## MCP Changes

Add tools:

- `juce_fill`
- `juce_clear`
- `juce_check`
- `juce_uncheck`
- `juce_set_checked`
- `juce_set_value`
- `juce_select_option`
- `juce_select_tab`
- `juce_scroll`
- `juce_select_row`

Each returns a fresh snapshot after success.

## Internal C++ Endpoint Changes

Implement semantic dispatch in layers:

1. Direct JUCE widget API.
2. Accessibility action/value/text/table/cell interface.
3. Synthesized input fallback only when safe and documented.

Control mappings:

- `juce::Button`: `triggerClick()`, `setToggleState()`, `getToggleState()`.
- `juce::TextEditor`: `setText()`, `clear()`, `insertTextAtCaret()`.
- `juce::Label`: `setText()` only when editable or mutation is explicitly
  allowed.
- `juce::Slider`: `setValue()`, `getValue()`, `getMinimum()`, `getMaximum()`,
  `getInterval()`.
- `juce::ComboBox`: `getNumItems()`, `getItemText()`, `getItemId()`,
  `setSelectedId()`, `setSelectedItemIndex()`, `setText()`.
- `juce::TabbedComponent`: `getTabNames()`, `setCurrentTabIndex()`.
- `juce::Viewport`: `setViewPosition()`, `setViewPositionProportionately()`.
- `juce::ListBox`: `selectRow()`, `selectRangeOfRows()`, `getSelectedRows()`.
- `juce::TableListBox`: phase 2 unless row behavior is fully covered by
  `ListBox` APIs in the concrete target.
- `juce::TreeView`: phase 2 unless the target is a component-backed item with a
  reliable accessibility action.

Notification semantics:

- User-like semantic actions must send JUCE notifications by default.
- Use synchronous notifications only where existing JUCE APIs make that the
  predictable user-like behavior.
- Query-only methods must not send notifications.
- Tests must prove listener callbacks fire for Button, Slider, ComboBox, and
  TextEditor changes.

## Fixture Additions

Add or expand controls:

- Toggle button with status update.
- Slider with min, max, interval, and status update.
- ComboBox with stable labels and ids.
- Editable TextEditor and read-only TextEditor.
- Editable Label if supported.
- TabbedComponent with named tabs.
- Viewport with scrollable content and status label.
- ListBox with selectable rows.
- TableListBox and TreeView only in phase 2 or quarantine fixtures.

## DemoRunner Scenarios

- Settings tab: select LookAndFeel ComboBox option.
- Settings tab: scroll viewport.
- AccessibilityDemo: check buttons and set sliders.
- FlexBoxDemo: fill numeric TextEditors, toggle radio-style buttons, select
  ComboBox options.
- MDIDemo: toggle options and create/select notes.

## Test Matrix

C++ fixture self-test:

- fill/clear text.
- check/uncheck/set_checked.
- set slider value and assert status/value.
- select ComboBox by label, id, and index.
- select tab by name and index.
- scroll viewport and assert offset.
- select row and assert selection.
- listener notification callbacks fire for user-like semantic actions.
- unsupported semantic action returns an error.

CLI coverage:

- one success and one failure per semantic command group.

MCP coverage:

- each added MCP tool appears in `tools/list` with required fields.
- representative text, toggle, slider, combo, and tab actions succeed.
- required-field validation and endpoint-error propagation are covered.

## Failure Modes

- `unsupported_control`
- `target_not_editable`
- `target_not_toggleable`
- `option_not_found`
- `tab_not_found`
- `row_out_of_range`
- `value_out_of_range`
- `semantic_action_failed`

## Acceptance Criteria

- Semantic actions do not require screenshots or coordinates.
- Public errors explain the unsupported control/action combination.
- Snapshot output includes enough semantic state to verify each action.
- DemoRunner validates the approach against real JUCE widgets.

## Evidence Artifacts

- C++ fixture logs final snapshot after each semantic section on failure.
- DemoRunner screenshots after settings, accessibility, and list/table flows.
