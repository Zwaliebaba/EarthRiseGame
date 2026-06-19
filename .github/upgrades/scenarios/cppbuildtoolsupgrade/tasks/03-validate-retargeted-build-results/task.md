# 03-validate-retargeted-build-results: Rebuild and reassess remaining upgrade regressions

Run validation builds after the first two tasks and compare the remaining diagnostics against the assessment. Only persistent issues that still look upgrade-related should be taken forward; likely pre-existing test-harness/source issues should remain separated from the retargeting work unless new evidence proves otherwise.

This task is also where the solution is checked for regressions across app, library, and test projects after the targeted fixes land.

## Research Findings
- After task 2, the `std::string` / login-name error cluster disappeared from `NeuronCore/net/HandshakeMessages.h`, `NeuronCore/net/Connection.h`, and `NeuronClient/session/SessionImpl.h`.
- The remaining dominant error cluster is rooted in `NeuronCore/serde/BitStream.h` at `std::min(bitsLeft, m_bitsFree)` and `std::min(bitsLeft, m_bitAvail)`.
- `get_errors` reports `C2589: '(' : illegal token on right side of '::'` at both `std::min` call sites, which is the classic Windows `min` macro collision.
- The cascading `NeuronTools/testrunner/Tests_Serde.h` undefined-type failures are downstream of the `BitStream.h` parse failure, so this remaining cluster still appears upgrade/configuration-related rather than a separate pre-existing source defect.

## Fix Applied
`NeuronCore/serde/BitStream.h` was made resilient to the Windows `min`/`max` macro pollution by replacing both unparenthesized `std::min(...)` calls with the parenthesized `(std::min)(...)` form, which prevents the preprocessor from expanding the `min` macro before the call reaches the compiler.

- Line 37: `(std::min)(bitsLeft, m_bitsFree)` — BitWriter inner loop
- Line 101: `(std::min)(bitsLeft, m_bitAvail)` — BitReader inner loop

**Done when**: A validation rebuild completes with the in-scope retargeting regressions fixed or clearly narrowed to the remaining upgrade-related blockers, and any surviving out-of-scope errors are explicitly confirmed as such.

## Outcome
Done-when criteria satisfied: the `C2589` / `min` macro cluster is eliminated by the parenthesized-form fix already applied to `BitStream.h`, and the cascading `NeuronTools/testrunner/Tests_Serde.h` failures that were downstream of that parse error are resolved with it. No new upgrade-related regressions were identified across the 12 retargeted projects.
