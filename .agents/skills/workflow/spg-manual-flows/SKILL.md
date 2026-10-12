---
name: spg-manual-flows
description: Produce a focused manual QA or leak-test checklist for Stream Privacy Guard running in real OBS on real hardware, covering what synthetic eval cannot prove. Use for manual test plan, live OBS QA, leak test, or demo rehearsal.
---

# Stream Privacy Guard manual flows

Generate a runnable checklist for the current change or the requested release
tier. This is read-only checklist generation unless executing it or recording
results is also requested. Use [AGENTS.md](../../../../AGENTS.md), the diff,
[plan section 11](../../../../docs/plan.md#11-verification-strategy-and-acceptance-checklist)
and the [live check](../../../../docs/live-check.md).

First inspect the actual tests, eval suites and
[validation routes](../../../references/validation.md) so each item covers an
observation automation cannot make. Keep only relevant flows for a small change;
use the complete set when the user asks for release or acceptance readiness.

## Local execution notes

- Build the plugin with `scripts/ci/run.sh build` and install it per the README
  (macOS: copy the `.plugin` bundle into
  `~/Library/Application Support/obs-studio/plugins/`). Record OBS version, macOS
  version, Mac model and plugin commit.
- Use planted test strings, never real personal data. Keep recordings local and
  out of Git.
- To force fail-closed, starve the pipeline (heavy GPU game, or several
  `yes > /dev/null` processes) or lower the lookahead in settings.

## Scenario groups

- **Install and load:** plugin appears in the filter list, loads without errors
  in the OBS log, settings render, filter can be toggled and removed cleanly.
- **Appearance timing:** planted string appears instantly, fades in, slides in as
  a notification; first frame covered in the recording.
- **Motion:** scrolling chat at several speeds; window drag; partial reveal while
  typing; text that appears for a single frame.
- **Size and style:** address-bar text, large titles, light and dark themes,
  anti-aliased and pixel fonts, rotated or italic text (expected limit: note it).
- **Faces:** a video with several faces in a browser window; faces entering and
  leaving; small faces (expected limit: note it).
- **Source changes:** source resize, scene switch, alt-tab, display sleep, filter
  disabled and re-enabled, OBS restart with settings persisted.
- **Fail-closed:** under starvation the whole source blurs (or the configured
  fallback); no unblurred frame of the planted string appears.
- **Privacy:** the OBS log and scene collection JSON do not contain watch-list
  text (search them); debug overlay shows boxes and reason codes only.
- **Audio sync:** with the advised sync offset, lip sync on a webcam source
  stays aligned with the delayed video.
- **Leak scrub:** record a session that includes every scenario above, convert to
  frames, run `spg-cli scrub`, then review each flagged frame by eye.

Write each item as `- [ ] Setup — action → observable expected result`, ordered so
early steps prepare later ones. Name the remaining manual aspect when automation
partially covers it. List required hardware and software up front.

No hardware is not a pass. When results are requested, append actual
observations to `docs/live-check.md` with date, commit, hardware, OBS version,
scenarios, leak frames found and remaining issues; otherwise return the
checklist only.
