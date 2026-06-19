# 02-fix-networking-header-breaks: Fix networking header compile breaks

Fix the first real source-level errors surfaced by the retargeted build in `NeuronCore/net/HandshakeMessages.h`, `NeuronCore/net/Connection.h`, and the dependent call site in `NeuronClient/session/SessionImpl.h`. The current error cluster points to missing direct standard-library/header dependencies and the resulting cascades around login-name handling.

The task should make the networking headers self-sufficient under the newer toolset and eliminate the resulting cascade errors without broad refactoring.

## Research Findings
- `NeuronCore/net/HandshakeMessages.h` declares `std::string username` in `LoginRequestBody` but does not include `<string>`.
- `NeuronCore/net/Connection.h` declares `std::string` members and `SetLoginName(std::string)` but does not include `<string>`.
- The `SessionImpl.h` call-site error is a cascade from the broken `ClientConnection::SetLoginName` declaration in `Connection.h`.
- `HandshakeMessages.h` also relies on `std::copy_n`, so tightening direct standard-library includes there reduces the chance of additional transitive-include breakage under the newer toolset.

## Planned Fix
Add the direct standard-library includes needed by the affected headers and then rebuild to confirm the `std::string` / login-name diagnostics are gone before addressing any subsequent error clusters.

**Done when**: The `std::string`/login-name related diagnostics are removed from the build and dependent projects compile past this cluster without introducing new upgrade-related warnings or errors.
