# 06. Waits And Assertions

## Goal

Add explicit waits that let agents and tests synchronize with JUCE UI state
without blind sleeps.

## Playwright Equivalent

Model the behavior after:

- `locator.waitFor()`
- `expect(locator).toHaveText()`
- `expect(locator).toHaveValue()`
- `expect(locator).toBeVisible()`
- waiting for UI changes after actions

## JUCE-Specific Differences

- Assertions are not a separate test runner API. They are endpoint methods used
  by C++ fixture tests, CLI commands, and MCP tools.
- Snapshot changes should compare serialized semantic state, not pointer
  identity.
- JUCE timers and async callbacks run on the message thread, so waits must keep
  polling without blocking that thread.

## Public Protocol Changes

Add methods:

- `wait_for_ref`
- `wait_for_locator`
- `wait_for_text`
- `wait_for_value`
- `wait_for_snapshot_change`

Common params:

```json
{
  "timeoutMs": 5000,
  "pollMs": 50,
  "state": "visible"
}
```

States:

- `attached`
- `detached`
- `visible`
- `hidden`
- `enabled`
- `disabled`
- `focused`
- `stable`

Text/value waits:

```json
{ "text": "Ready", "exact": true }
{ "value": "75" }
```

Snapshot change waits:

```json
{
  "sinceStateHash": "b4b1...",
  "mode": "semantic"
}
```

`generation` is only a ref namespace and must not be used as proof of UI state
change.

## CLI Changes

Add:

```sh
melatonin-ui -s app wait-for-locator --role button --name Save --state visible --timeout 5000
melatonin-ui -s app wait-for-text "Ready" --timeout 5000
melatonin-ui -s app wait-for-value --component-name controls.slider 75
melatonin-ui -s app wait-for-snapshot-change --since-state-hash b4b1...
```

Successful waits print the final snapshot or matched node depending on
`--format`.

## MCP Changes

Add tools:

- `juce_wait_for`
- `juce_wait_for_text`
- `juce_wait_for_value`
- `juce_wait_for_snapshot_change`

Tool descriptions should encourage waits instead of `juce_wait` sleeps.

## Internal C++ Endpoint Changes

Add a generic polling primitive:

- accepts a predicate.
- dispatches state capture to the message thread.
- sleeps on the endpoint thread.
- returns final state or timeout diagnostics.

Use it for:

- locator waits.
- actionability waits.
- text/value waits.
- snapshot generation/state changes.

Snapshot change comparison:

- Compare canonical semantic JSON through the shared `stateHash`.
- Treat generation only as a ref namespace.
- Use the shared timeout hierarchy from
  [00-protocol-and-snapshot-model.md](00-protocol-and-snapshot-model.md).

## Fixture Additions

Add:

- timer-updated label.
- delayed attach component.
- delayed detach component.
- delayed slider/value update.
- delayed tab switch.
- hidden-to-visible component.

## DemoRunner Scenarios

- Wait for Browse Demos panel.
- Wait for category row text.
- Wait for selected demo content to appear.
- Wait for Settings tab content.
- Wait for dialog text in WindowsDemo.

## Test Matrix

C++ fixture self-test:

- wait for visible locator.
- wait for hidden/detached locator.
- wait for text exact and fuzzy.
- wait for value.
- wait for focused component.
- wait for snapshot change.
- repeated snapshots without UI changes do not satisfy snapshot-change waits.
- timeout case for every wait family.

CLI coverage:

- each wait command success and timeout.

MCP coverage:

- each wait tool listed and representative success/timeout.

## Failure Modes

- `wait_timeout`
- `invalid_wait_state`
- `invalid_state_hash`
- `locator_not_found`
- `value_mismatch`
- `text_mismatch`

## Acceptance Criteria

- New code paths stop using arbitrary sleeps except for polling intervals.
- Wait failures include last observed state.
- Waits can be used by actionability and public tools.
- C++ tests prove both success and timeout paths.
- Snapshot-change waits are based on semantic state, not snapshot generation.

## Evidence Artifacts

- Final observed snapshot in timeout diagnostics.
- CI failure logs include elapsed time and predicate summary.
