# Repository Guidelines

## Project Structure & Module Organization

This is a C++20 CMake project. The root `CMakeLists.txt` enables C++20, exports compile commands, and adds `src` and `test`.

- `src/core/`: static `core` library sources and public headers.
- `src/core/*_priv.hpp`: private implementation headers.
- `src/core/event_loop_backend_epoll.cpp` and `event_loop_backend_kqueue.cpp`: platform-specific event loop backends selected by CMake.
- `src/db`: database-related code.
- `src/db/sqlite`: SQLite database backend code.
- `src/jobu`: job scheduling and execution code.
- `src/jobuctl`: job control and command-line interface code.
- `src/jobud`: JobU daemon code.
- `src/net`: networking code.
- `src/rpc`: JSON-RPC code.
- `test/`: Catch2 test executables, one file per feature, named `*-test.cpp`.
- `.bld/`: local out-of-source build directory; do not commit generated build output.

## Stepwise implementation rule

For code changes, Codex must use a staged workflow.

- First propose a numbered plan before editing.
- Wait for approval before the first edit.
- Implement exactly one stage at a time.
- After each stage, stop and report changed files, validation, and the next proposed stage.
- Do not continue until the user explicitly approves.
- If new information invalidates the plan, stop and ask for approval of a revised plan.

## Build, Test, and Development Commands

- `cmake -B .bld -DCMAKE_BUILD_TYPE=Debug`: configure a Debug build.
- `cmake --build .bld`: build the `core` static library and all test executables.
- `ctest --test-dir .bld/test --output-on-failure`: run the registered Catch2 tests and show failures.
- `clang-format -i src/core/*.hpp src/core/*.cpp test/*.cpp`: format changed C++ files using the repository style.

The project requires CMake 3.20+, a C++20 compiler, `fmt`, `sqlite3`, `nlohmann_json`, and Catch2
discoverable via CMake package config.

## Clangd Diagnostics

When `.bld/compile_commands.json` is available, use the clangd MCP/LSP tools for C++ semantic
exploration and diagnostics.

- The repository has an established diagnostics-clean baseline. For normal work, query diagnostics
  only for changed C++ source files and changed standalone C++ headers; do not repeat a project-wide
  scan unless the user explicitly requests one or there is evidence that the baseline is no longer
  valid.
- Diagnose a changed intentionally non-standalone or platform-specific header through an owning
  translation unit with the matching target configuration. Report unavailable target coverage
  instead of treating fallback-context parser errors as code defects.
- Treat every enabled compiler, include-cleaner, and clang-tidy diagnostic reported for those files
  as a completion blocker. Fix all diagnostics within the approved scope; do not silently accept or
  suppress them.
- If a fix would expand the approved stage, or a diagnostic conflicts with the intended design or
  project style, stop and report the exact location, check name, message, and rationale to the user.
- Preserve intentional indirect includes with `// IWYU pragma: keep <justification>`. Use
  `// IWYU pragma: export` when a public header deliberately re-exports another header, and remove
  genuinely unused includes. Do not add filename-based `.clangd` exceptions for include intent.
- Do not modify `.clangd` or add other local diagnostic suppressions without explicit approval.
- Re-run diagnostics for the changed files after fixes and report the stage complete only when those
  files are clean.

## Coding Style & Naming Conventions

Use the checked-in `.clang-format`. Important defaults are 4-space indentation, 120-column limit, left-aligned pointers, and custom brace wrapping with function braces on their own line. Keep public API headers in `src/core` and implementation details in `.cpp` or `*_priv.hpp` files.

Follow existing naming patterns: snake_case file names such as `event_loop.cpp`, PascalCase types such as `EventLoop`, and lowerCamelCase or descriptive method names as already used in nearby code. Prefer small, focused classes and keep platform-specific code isolated behind backend files.

## Object Private Data and Signals

An `Object` subclass with private instance state must extend the single private block owned by `Object`. Derive its
private structure directly or transitively from `jb::core::priv::ObjectPrivate` and pass that one heap allocation to
the protected `Object` constructor. Do not add a second pimpl pointer or direct private implementation fields. Public
`Signal` members and static process-wide state may remain direct class members.

Do not pass a usable derived owner into a private-data constructor before the `Object` base is constructed. When
private implementation code needs the public owner, bind the owner back-reference in the derived constructor body
after `Object` has taken ownership of the private block, then install any connections that require the owner.

Use a signal for a reusable observable event emitted by an `Object`. Use a receiver-aware connection whenever a slot
captures an `Object`, so receiver destruction deactivates the slot. Reserve context-free connections for callables
that borrow no `Object`, borrow only process-lifetime objects, or are explicitly disconnected before every captured
target can be destroyed. Keep callbacks for one accepted operation's completion, strategies that produce a return
value, and private non-`Object` adapter seams.

Treat useful public Doxygen and concise rationale comments at non-obvious ownership, lifetime, ordering, failure, and
reentrancy boundaries as stage completion requirements.

## Documentation & Comments

Document public APIs with Doxygen-style comments. In `.cpp` files, document multi-step method/function bodies
when the implementation is not self-documenting. Focus internal comments on intent, rationale, ordering,
invariants, and failure behavior. Avoid comments that merely restate the function name or individual statements;
no body comment is needed when the name and implementation already make the behavior clear.

## Testing Guidelines

Tests use Catch2 with `Catch2::Catch2WithMain`. Add new tests under `test/` as `feature-test.cpp`, register the executable in `test/CMakeLists.txt`, link it with `core` and Catch2, and add it with `add_test`. Keep tests behavior-focused and cover event loop, timer, object lifetime, signal, and threading changes with deterministic assertions.

## Commit & Pull Request Guidelines

Use a short subject that describes the behavior change; avoid vague subjects except for temporary local work. The subject implicitly includes "This commit changes the software to" prefix. The description explains the rationale, design decisions, and any relevant context.

Do not commit yourself, only suggest a commit message when asked.
