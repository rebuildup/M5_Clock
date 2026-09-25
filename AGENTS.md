# AGENTS.md — M5_Clock

## Governance
- Constitution: [`constitution/CONSTITUTION.md`](constitution/CONSTITUTION.md)
- Operating Model: [`organization/profiles/release-driven-solo.md`](organization/profiles/release-driven-solo.md)

## Firmware / device boundaries
- Arduino/M5Stack board configuration and M5Unified are project toolchain authority.
- Source/compile evidence is separate from flashed-device evidence.
- Record board model, firmware candidate/source SHA, and relevant runtime observation for hardware validation.
- Never commit `credentials.h` or reveal local Wi-Fi credentials.
- Do not treat network/NTP availability failures as proof of unrelated timer/UI defects.

## Delivery
- durable work: GitHub Issue.
- ticket branch: Issue number.
- ticket PR: current release branch.
- normal main integration: release PR only.
- landing: merge commit only.
