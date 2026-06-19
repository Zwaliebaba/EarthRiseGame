# 03-validate-retargeted-build-results: Rebuild and reassess remaining upgrade regressions

Run validation builds after the first two tasks and compare the remaining diagnostics against the assessment. Only persistent issues that still look upgrade-related should be taken forward; likely pre-existing test-harness/source issues should remain separated from the retargeting work unless new evidence proves otherwise.

This task is also where the solution is checked for regressions across app, library, and test projects after the targeted fixes land.

## Research Findings
- After task 2, the `std::string` / login-name error cluster disappeared from `NeuronCore/net/HandshakeMessages.h`, `NeuronCore/net/Connection.h`, and `NeuronClient/session/SessionImpl.h`.
- The remaining dominant error cluster is rooted in `NeuronCore/serde/BitStream.h` at `std::min(bitsLeft, m_bitsFree)` and `std::min(bitsLeft, m_bitAvail)`.
- `get_errors` reports `C2589: '(' : illegal token on right side of '::'` at both `std::min` call sites, which is the classic Windows `min` macro collision.
- The cascading `NeuronTools/testrunner/Tests_Serde.h` undefined-type failures are downstream of the `BitStream.h` parse failure, so this remaining cluster still appears upgrade/configuration-related rather than a separate pre-existing source defect.

## Planned Fix
Make `NeuronCore/serde/BitStream.h` resilient to Windows `min` macro pollution and re-run validation builds to determine whether any true out-of-scope issues remain afterward.

**Done when**: A validation rebuild completes with the in-scope retargeting regressions fixed or clearly narrowed to the remaining upgrade-related blockers, and any surviving out-of-scope errors are explicitly confirmed as such.
