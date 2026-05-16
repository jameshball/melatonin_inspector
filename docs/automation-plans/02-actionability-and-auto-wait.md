# 02. Actionability And Auto-Wait

## Goal

Make actions reliable by waiting until targets are ready, matching the parts of
Playwright actionability that map cleanly to JUCE.

## Playwright Equivalent

Playwright auto-waits before actions and checks that a target is visible,
stable, receives events, enabled, and editable when required.

## JUCE-Specific Differences

- JUCE has no layout engine or animation lifecycle equivalent to the browser.
  Stability should be measured by component bounds and visibility across polls.
- "Receives events" means root-local hit testing reaches the target or a child
  that belongs to the target.
- Some valid semantic actions do not need hit testing. For example,
  `Slider::setValue()` and `ComboBox::setSelectedId()` can succeed when a mouse
  click would not be the best implementation.
- Offscreen or hidden targets may still be useful for inspector mutation, but
  user-like actions should reject them unless `force` is set.

## Public Protocol Changes

Add common action parameters:

```json
{
  "timeoutMs": 5000,
  "force": false,
  "trial": false
}
```

Actionability states:

- `attached`
- `visible`
- `showing`
- `enabled`
- `stable`
- `receivesEvents`
- `editable`

`trial: true` runs locator resolution and actionability checks without
performing the action.

Trial response:

```json
{
  "trial": true,
  "target": { "ref": "m3-4", "name": "Save", "role": "button" },
  "checks": {
    "attached": true,
    "visible": true,
    "enabled": true,
    "stable": true,
    "receivesEvents": true
  }
}
```

Trial never performs the action and never returns a post-action snapshot.

Default timeout:

- 5000 ms for locator actions.
- 0 ms for ref actions unless `timeoutMs` is supplied.
- Existing fixed `wait --ms` behavior remains for compatibility.
- CLI/MCP process timeouts must exceed endpoint timeouts by at least 1000 ms.
- If a client disconnects during a wait, the endpoint should cancel the pending
  poll before dispatching more message-thread work.

## CLI Changes

Add common flags to target-taking commands:

```sh
melatonin-ui -s app click --role button --name Save --timeout 5000
melatonin-ui -s app click --role button --name Save --force
melatonin-ui -s app click --role button --name Save --trial
```

On failure, CLI stderr should show:

- locator/ref
- action name
- failed actionability check
- last observed target state

## MCP Changes

Extend action tools with:

- `timeoutMs`
- `force`
- `trial`

Tool errors should be plain enough for an LLM to use:

```text
Target matched "Save" but is disabled.
```

When useful, include the last known target summary as JSON text content.

## Internal C++ Endpoint Changes

Add:

- `ActionabilityOptions`
- `ActionabilityResult`
- `waitForTarget`
- `checkActionability`

Polling:

- Use endpoint worker thread for polling.
- Dispatch each poll to the JUCE message thread.
- Poll every 50 ms by default.
- Stop immediately if the endpoint is shutting down.
- Keep timeout accounting outside the message thread.
- Use the shared timeout budget from
  [00-protocol-and-snapshot-model.md](00-protocol-and-snapshot-model.md).

Checks:

- attached: `SafePointer` still valid.
- visible: `Component::isVisible()`.
- showing: `Component::isShowing()`.
- enabled: `Component::isEnabled()`.
- non-empty bounds: root bounds width and height greater than zero.
- stable: root bounds unchanged across two consecutive polls.
- receives events: center point hit test resolves to the target or descendant.
- editable: `TextEditor`, editable `Label`, editable `ComboBox`, or a component
  with writable accessibility value/text interface.

## Fixture Additions

Add controls:

- Button that appears after a timer.
- Button that enables after a timer.
- Component that moves briefly before settling.
- Disabled button.
- Hidden button.
- Obscured button.
- Read-only and editable text controls.

## DemoRunner Scenarios

After DemoRunner support exists:

- Wait for the Browse Demos side panel to appear.
- Wait for category rows after opening the panel.
- Wait for a selected demo tab to become visible.
- Validate disabled or non-actionable controls in settings if present.

## Test Matrix

C++ fixture self-test:

- Locator click waits for delayed attach.
- Locator click waits for delayed enabled.
- Moving target action waits for stable bounds.
- Disabled target fails.
- Hidden target fails.
- Obscured target fails receives-events.
- `force` bypasses visibility/receives-events where safe.
- `trial` validates without changing state.
- `trial` response includes target summary and check results.
- Timeout produces deterministic error code.

CLI coverage:

- `--timeout`
- `--force`
- `--trial`

MCP coverage:

- action succeeds after delayed readiness.
- action failure returns useful diagnostics.

## Failure Modes

- `action_timeout`
- `target_detached`
- `target_not_visible`
- `target_not_showing`
- `target_disabled`
- `target_not_stable`
- `target_not_editable`
- `target_not_receiving_events`
- `trial_failed`

## Acceptance Criteria

- Locator actions are retryable and stable.
- Existing ref actions keep current behavior unless timeout is supplied.
- Failures identify the failed actionability check.
- `force` is explicit and tested.
- Tests prove both successful waiting and timeout behavior.

## Evidence Artifacts

- Last target state JSON in failure logs.
- Screenshot on actionability failure once screenshot artifacts are available.
