# Code Standards

## Language and Compiler

| Setting | Value |
|---|---|
| **Standard** | Use the project's configured C++ standard; prefer a modern standard such as C++20 or later for new code. |
| **Compiler** | Use the compiler configured by the project or build system. |
| **Platform** | Follow the target platforms supported by the project. |
| **Exceptions** | Use consistently with the project's error-handling strategy; avoid exceptions in hot paths unless the project explicitly allows them. |
| **RTTI** | Use only when it improves clarity; prefer explicit type modeling such as `enum class`, variants, or virtual dispatch where appropriate. |
| **STL** | Prefer standard library containers, algorithms, smart pointers, and utilities. |
| **C++ features** | Use modern C++ features when they improve safety, clarity, or maintainability. |

Keep compiler settings, warning levels, and language standards consistent across project targets. Avoid introducing settings that only work for one developer environment unless they are documented and automated.

---

## Naming Conventions

### Classes and Types

Use `PascalCase`:

```cpp
class Entity;
class Renderer;
struct Vector3;
class Application;
```

### Member Variables

Prefix private or protected member variables with `m_` and use `camelCase`:

```cpp
class Entity
{
  EntityType m_entityType;
  int m_instanceId;
  float m_updateInterval;
  bool m_isActive;
};
```

### Functions and Local Variables

Use `camelCase`:

```cpp
void update(Entity* entity);
int itemCount = 0;
bool isVisible = true;
float elapsedTime = 0.0f;
```

### Constants and Macros

Use `UPPER_SNAKE_CASE` for macros and all constants, including `constexpr` values:

```cpp
#define MAX_PLAYERS 8
#define UPDATE_INTERVAL_SECONDS 0.1f

constexpr int DEFAULT_BUFFER_SIZE = 1024;
constexpr float DEFAULT_TIMEOUT_SECONDS = 30.0f;
```

### Enums

Use `enum class` with explicit underlying types when storage, serialization, ABI, or interop matters:

```cpp
enum class EntityType : int8_t
{
  Invalid = -1,
  Player,
  Enemy,
  Neutral
};
```

### File Names

Use the naming style established by the project. Common patterns include:

- `PascalCase.cpp` / `PascalCase.h` for types and modules.
- `snake_case.cpp` / `snake_case.h` when matching an existing codebase.

Match the existing pattern in the directory being edited.

---

## File Organization

### Header Guards

Prefer `#pragma once` for new code unless the project requires traditional include guards:

```cpp
#pragma once

#include <string>
// ... rest of header
```

Preserve existing guard style when editing existing files:

```cpp
#ifndef INCLUDED_MODULE_H
#define INCLUDED_MODULE_H
// ... rest of header
#endif
```

### Include Order

Use a consistent include order. A common order is:

```cpp
#include "pch.h"              // Precompiled header first, if required by the project

#include "CurrentHeader.h"    // Corresponding header in .cpp files

#include "ProjectHeader.h"    // Project headers

#include <vector>              // Standard library headers
#include <cstdint>

#include <PlatformHeader.h>    // Platform or SDK headers
```

- One primary concept per `.cpp`/`.h` pair where practical.
- Keep headers minimal; forward-declare where possible and include implementation details in `.cpp` files.
- If the project uses precompiled headers, include the configured precompiled header first in `.cpp` files.
- Avoid relying on transitive includes.

---

## Comments

Use C++ style comments for most documentation:

```cpp
// Single-line comments for brief explanations

/* Multi-line comments for longer explanations,
   file headers, or section dividers */

/// Optional: documentation comments for public APIs
/// @param item The item to process
/// @return true if successful
bool process(Item* item);
```

- Comment the **why**, not the **what**.
- Avoid comments that restate the code.
- Keep function-level comments brief.
- Add inline explanations for complex logic, non-obvious tradeoffs, or important invariants.
- Keep comments updated when behavior changes.

---

## Memory Management

### Ownership

Prefer RAII and smart pointers for ownership:

```cpp
std::unique_ptr<Resource> resource;
std::shared_ptr<Service> sharedService;
```

- Use `std::unique_ptr<T>` for exclusive ownership.
- Use `std::shared_ptr<T>` only when shared ownership is required.
- Use `std::weak_ptr<T>` to break ownership cycles.
- Use raw pointers and references for non-owning access.
- Avoid manual `new` and `delete` in new code unless required by an API or existing project pattern.

### Allocation and Deallocation Helpers

If the project defines allocation, deallocation, or debug tracking helpers, use them consistently in files that already follow that pattern. Do not introduce new memory macros unless there is a clear project-wide need.

### Containers

Prefer standard containers such as `std::vector`, `std::array`, `std::unordered_map`, and `std::string`. Use custom containers only when required by project constraints, interoperability, or measured performance needs.

---

## Platform and Architecture

### Platform Boundaries

Keep platform-specific code isolated behind clear abstractions:

- Put platform, SDK, or operating system calls in platform-specific modules.
- Keep domain logic independent from UI, rendering, storage, networking, and operating system APIs where practical.
- Avoid leaking platform-specific types into public interfaces unless that interface is explicitly platform-specific.

### Layers and Dependencies

Respect the dependency direction established by the project:

- Lower-level libraries should not depend on higher-level application code.
- Shared utilities should avoid dependencies on feature-specific modules.
- Tests may depend on production modules, but production modules should not depend on tests.
- Avoid circular dependencies between modules.

Document new architectural boundaries when adding them.

---

## Coding Patterns

### Classes and Structs

Use `class` for types with behavior or invariants. Use `struct` for simple data aggregates:

```cpp
class Entity
{
public:
  bool update(float deltaTime);

private:
  bool m_isActive = true;
};

struct EntityId
{
  int value = 0;
};
```

Keep constructors explicit when a single-argument constructor could introduce unintended conversions:

```cpp
class EntityId
{
public:
  explicit EntityId(int value);
};
```

### Error Handling

Use the error-handling style established by the project. Common options include:

- Return values such as `bool`, error codes, or result types.
- Exceptions for exceptional failures when allowed by the project.
- Assertions for programmer errors and invariants.
- Logging for diagnostic information.

Guidelines:

- Validate inputs when failure is possible in normal use.
- Use assertions for conditions that indicate a programming bug.
- Avoid swallowing errors silently.
- Include enough context in logs to diagnose the issue.

### Modern C++ Features

Use modern C++ features where they improve clarity and safety:

```cpp
for (const auto& item : items)
{
  process(item);
}

if (auto it = values.find(key); it != values.end())
{
  use(it->second);
}
```

Prefer:

- Range-based loops and standard algorithms.
- `auto` when it improves readability or avoids repeated complex types.
- `constexpr` and `const` for values that should not change.
- `std::optional`, `std::variant`, and result-like types for explicit state modeling.
- `std::span` or views for non-owning ranges when supported by the project standard.

Avoid using advanced language features solely for novelty.

### Loops and Performance-Sensitive Code

For standard containers, prefer range-based loops or algorithms. For indexed access or custom containers, follow existing project patterns:

```cpp
for (std::size_t i = 0; i < items.size(); ++i)
{
  process(items[i]);
}
```

Avoid unnecessary allocations in hot paths. Optimize based on measurement, not assumption.

---

## Project Structure

### Layout

Each repository should document its own layout separately. A common layout is:

| Directory | Purpose |
|---|---|
| `src/` | Production source code |
| `include/` | Public headers, if used |
| `tests/` | Unit and integration tests |
| `docs/` | Project documentation |
| `tools/` | Build, generation, and maintenance scripts |
| `assets/` | Runtime assets or test data, if applicable |

Keep source files close to the feature or module they implement. Avoid adding broad utility modules unless the utility is genuinely shared.

### Adding New Files

When adding files:

- Place them in the module or directory that owns the behavior.
- Add them to the build system and IDE/project files as required.
- Keep project filters or folder views in sync when applicable.
- Include tests for new behavior where the project has a test framework.
- Follow the surrounding file naming and include conventions.

---

## Build Conventions

### Building

Use the repository's documented build system and scripts. Keep build instructions in a project-specific document such as `README.md`, `BUILD.md`, or developer setup documentation.

Do not hard-code local paths, machine-specific settings, or user-specific environment assumptions in shared build files.

### Compiler Settings

Recommended defaults for new code:

- High warning level enabled.
- Warnings treated consistently according to project policy.
- Modern C++ standard enabled.
- Debug information enabled for debug builds.
- Optimization enabled for release builds.
- Sanitizers, static analysis, or code analysis enabled where supported.

### Warnings and Errors

- Maintain a zero-new-warnings policy.
- Prefer fixing warnings over suppressing them.
- If a warning must be suppressed, keep the suppression narrow and document why.
- Do not disable warnings globally for convenience.

---

## Debugging and Logging

Use the project's logging and assertion facilities consistently:

```cpp
assert(pointer != nullptr);
logDebug("Starting update");
```

Guidelines:

- Use assertions for programmer errors and invariants.
- Use logs for runtime diagnostics.
- Avoid excessive logging in performance-sensitive paths.
- Do not log secrets, credentials, tokens, or personally identifiable information.
- Prefer structured or categorized logging when supported by the project.

---

## Testing and Validation

### Testing Strategy

Follow the project's test framework and test naming conventions. Prefer tests that are:

- Focused on one behavior.
- Deterministic and repeatable.
- Fast enough to run during development.
- Clear about expected outcomes.

### Validation Checklist

Before submitting changes:

- Build the affected targets.
- Run relevant unit tests.
- Run integration or manual validation when behavior crosses module boundaries.
- Check for new compiler warnings, static analysis findings, or formatting issues.
- Update documentation when behavior, APIs, or setup steps change.

---

## Version Control and Commits

### Git Commit Style

Use imperative mood, present tense:

```text
Add input validation for configuration parsing
Fix cache invalidation after settings update
Refactor renderer initialization
Update build documentation
```

- Keep the subject line concise, preferably under 72 characters.
- Make one logical change per commit.
- Explain motivation and tradeoffs in the body when they are not obvious.
- Reference relevant issues, modules, or files when helpful.

### Branch Strategy

Follow the repository's branching and review policy. In general:

- Keep branches focused and short-lived.
- Rebase or merge regularly to reduce integration risk.
- Use pull requests or code reviews when required by the team.

---

## Additional Guidelines

### When Adding Features

1. Check for existing utilities, patterns, and extension points before adding new ones.
2. Keep behavior in the module that owns it.
3. Update related interfaces, tests, and documentation together.
4. Preserve backward compatibility unless the change intentionally breaks it and is documented.
5. Prefer small, observable changes that can be reviewed and tested independently.

### When Debugging

1. Reproduce the issue before changing code when practical.
2. Isolate the smallest failing behavior.
3. Add temporary diagnostics only when needed, and remove them before submitting unless they are useful permanent logs.
4. Validate the fix with a targeted test or documented manual scenario.

### When Refactoring

1. Preserve existing behavior.
2. Keep refactors separate from behavior changes when possible.
3. Update names, comments, and documentation to match the new structure.
4. Ensure the project builds and tests still pass.

---

## Summary Checklist

When writing new code, ensure:

- [ ] Names follow the project's conventions.
- [ ] Headers are minimal and include only what they need.
- [ ] `.cpp` files include the configured precompiled header first when required.
- [ ] Ownership is explicit and RAII is preferred.
- [ ] Platform-specific code is isolated behind appropriate abstractions.
- [ ] Errors are handled consistently and not ignored silently.
- [ ] Logs and assertions use project-approved facilities.
- [ ] New code introduces no avoidable warnings.
- [ ] Relevant tests or validation steps have been run.
- [ ] Documentation is updated when conventions, behavior, or setup changes.

---

*Keep these standards up to date as conventions evolve. Project-specific build steps, architecture diagrams, dependencies, and module lists should live in repository-specific documentation.*
