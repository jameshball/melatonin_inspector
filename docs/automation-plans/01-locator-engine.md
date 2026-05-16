# 01. Locator Engine

## Goal

Add Playwright-style semantic target lookup so callers can act on JUCE controls
by role, visible text, component id, component name, value, class, or position in
the match set.

Refs remain supported, but locator-based actions should be preferred because
they resolve against the current component tree and do not depend on a previous
snapshot generation.

This feature depends on [00-protocol-and-snapshot-model.md](00-protocol-and-snapshot-model.md).

## Playwright Equivalent

Model the behavior after Playwright locators:

- `getByRole()`
- `getByText()`
- `getByTestId()`
- `locator().filter()`
- strict single-target actions
- `nth()` for explicit disambiguation

## JUCE-Specific Differences

- JUCE has no DOM. The resolver walks `juce::Component` children and selected
  accessibility children where available.
- `Component::getComponentID()` is the default equivalent of Playwright test id.
- `Component::getName()` remains separately queryable as `componentName`.
- The accessible title, component string, label text, button text, and value all
  contribute to semantic text matching.
- Some JUCE list and tree rows are accessibility children rather than direct
  `Component` children; the first implementation should support component-backed
  controls and explicitly mark pure accessibility-only children as follow-up if
  they cannot be safely acted on.

## Public Protocol Changes

Add a `locator` object accepted by target-taking methods:

```json
{
  "role": "button",
  "name": "Go Editor",
  "text": "Go Editor",
  "componentId": "main.goEditor",
  "componentName": "nav.editor",
  "testId": "main.goEditor",
  "class": "juce::TextButton",
  "value": "25",
  "exact": true,
  "visible": true,
  "enabled": true,
  "focused": false,
  "hasText": "Editor",
  "nth": 0
}
```

Resolution rules:

- If `ref` is supplied, use ref resolution first.
- If `locator` is supplied, resolve against a fresh tree.
- Supplying both `ref` and `locator` is an error unless a future compatibility
  mode explicitly allows locator fallback.
- Locators default to `visible: true` for actions and `visible: false` for
  snapshot/query-only calls.
- Text matching is Playwright-like by default: normalize whitespace and perform
  case-insensitive substring matching for `text` and `hasText`.
- `exact: true` switches string fields to full normalized-string comparison.
- Regex locators are explicitly out of scope for v1 and should be added only
  after string matching is stable.
- `testId` aliases `componentId`.
- Actions require exactly one match.
- Multiple matches return `strict_mode_violation` with a compact list of
  matching refs/names/classes.
- Zero matches return `locator_not_found` after the configured timeout.

Add endpoint methods:

- `locator`
- `count`
- `describe`

`locator` returns the matched nodes with refs, names, roles, bounds, and values.
`count` returns only the count. `describe` returns a single strict match.

## CLI Changes

Add:

```sh
melatonin-ui -s app locator --role button --name "Go Editor"
melatonin-ui -s app locator --text "Status:" --format json
melatonin-ui -s app click --role button --name "Go Editor"
melatonin-ui -s app click --test-id main.goEditor
melatonin-ui -s app click --component-name nav.editor
melatonin-ui -s app click --text "Reset All" --nth 0
```

Rules:

- Existing `click <ref>` keeps working.
- Locator flags are available on all actions that currently take a ref.
- `--timeout` controls locator retry.
- `--all` is valid only for query commands, not strict actions.

## MCP Changes

Add tool:

- `juce_locator`

Extend all action tools to accept:

```json
{
  "session": "app",
  "locator": { "role": "button", "name": "Go Editor" },
  "timeoutMs": 5000
}
```

MCP responses should include compact JSON for locator queries. For actions,
return a fresh text snapshot by default and JSON when requested.

## Internal C++ Endpoint Changes

Implement shared types inside the automation implementation:

- `AutomationLocator`
- `ResolvedTarget`
- `LocatorMatch`
- `LocatorError`

Add helpers:

- parse locator from `juce::DynamicObject`
- serialize locator in error messages
- collect component tree
- collect searchable text for a component
- derive role/title/value
- match exact or substring text
- apply visible/enabled/focused filters
- apply `nth`
- produce strictness diagnostics

Do not duplicate locator parsing in CLI or MCP. Clients should only serialize
locator fields; endpoint owns target resolution.

## Fixture Additions

Extend `tests/automation_fixture.cpp`:

- Set `Component::setComponentID()` on all important controls.
- Add duplicate buttons to test strict mode.
- Add hidden and disabled controls.
- Add labels with repeated and whitespace-heavy text.
- Add a value-bearing slider and combo.
- Add a focused text editor case.

## DemoRunner Scenarios

After DemoRunner support exists:

- Locate `Browse Demos` by role/name.
- Locate side panel rows by text.
- Locate the Demo, Code, and Settings tabs by role/name or text.
- Locate settings ComboBoxes by role and nearby label text where possible.
- Locate AccessibilityDemo buttons/sliders by role.

## Test Matrix

C++ fixture self-test:

- Query by role.
- Query by exact and fuzzy text.
- Query by component id/test id.
- Query by component name.
- Query by class.
- Query by value.
- Hidden filtering.
- Disabled filtering.
- Focused filtering.
- `nth` disambiguation.
- zero match timeout.
- multiple match strict failure.
- stale ref remains a stale ref error.

CLI coverage:

- `locator --format json`
- `click` with locator flags
- strict failure exit code and message

MCP coverage:

- `tools/list` includes `juce_locator`
- `juce_locator` returns expected matches
- `juce_click` accepts a locator
- strict failure returns JSON-RPC tool error text

## Failure Modes

- `invalid_locator`: locator schema is invalid.
- `unsupported_locator`: field is known but not implemented for this target.
- `locator_not_found`: no match before timeout.
- `strict_mode_violation`: more than one match without `nth`.
- `stale_ref`: ref exists only in an old snapshot generation.
- `target_not_showing`: locator matched but actionability failed.

## Acceptance Criteria

- All ref-based behavior keeps working.
- All new target-taking commands accept locators.
- Locator errors are stable enough for C++ assertions.
- CLI and MCP both use endpoint locator resolution.
- The fixture proves strictness, matching, and failure behavior.

## Evidence Artifacts

- JSON locator output captured by the fixture on failure.
- Fresh snapshot text included in locator action failures.
- CI logs include strictness diagnostics.
