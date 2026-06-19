# C++ Build Tools Upgrade Plan

## Overview

**Target**: Stabilize the EarthRise Visual Studio 2026 C++ solution after retargeting all projects to MSVC v145 and fix the upgrade-related build breaks exposed by the newer toolset.
**Scope**: Large native solution with 12 VC++ projects, shared networking headers, app/test projects, and one solution-wide IDE build-orchestration blocker.

## Tasks

### 01-stabilize-solution-build: Stabilize solution build orchestration

Resolve the Visual Studio 2026 solution/session build failure that reports `BeginBuild has not yet been called` across all projects. This task focuses on solution-level and project-level configuration checks that affect whether the IDE build pipeline can produce reliable diagnostics after retargeting.

This includes validating the retargeted project metadata in the reopened solution session and using the available build tools to confirm that assessment and execution can rely on repeatable build results.

**Done when**: The solution can be built through the supported build tools without the solution-wide `BeginBuild has not yet been called` failure, or the failure is isolated as an IDE/tooling-only issue with an alternate reliable validation path established for the remaining upgrade work.

---

### 02-fix-networking-header-breaks: Fix networking header compile breaks

Fix the first real source-level errors surfaced by the retargeted build in `NeuronCore/net/HandshakeMessages.h`, `NeuronCore/net/Connection.h`, and the dependent call site in `NeuronClient/session/SessionImpl.h`. The current error cluster points to missing direct standard-library/header dependencies and the resulting cascades around login-name handling.

The task should make the networking headers self-sufficient under the newer toolset and eliminate the resulting cascade errors without broad refactoring.

**Done when**: The `std::string`/login-name related diagnostics are removed from the build and dependent projects compile past this cluster without introducing new upgrade-related warnings or errors.

---

### 03-validate-retargeted-build-results: Rebuild and reassess remaining upgrade regressions

Run validation builds after the first two tasks and compare the remaining diagnostics against the assessment. Only persistent issues that still look upgrade-related should be taken forward; likely pre-existing test-harness/source issues should remain separated from the retargeting work unless new evidence proves otherwise.

This task is also where the solution is checked for regressions across app, library, and test projects after the targeted fixes land.

**Done when**: A validation rebuild completes with the in-scope retargeting regressions fixed or clearly narrowed to the remaining upgrade-related blockers, and any surviving out-of-scope errors are explicitly confirmed as such.
