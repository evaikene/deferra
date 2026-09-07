# Repository Guidelines

## Project Structure & Module Organization

This is a C++20 CMake project. The root `CMakeLists.txt` enables C++20, exports compile commands, and adds `src` and `test`.

* `src/core/`: static `core` library sources and public headers.
* `src/core/*_priv.hpp`: private implementation headers.
* `src/core/event_loop_backend_epoll.cpp` and `event_loop_backend_kqueue.cpp`: platform-specific event loop backends selected by CMake.
* `src/db`: database-related code.
* `src/db/sqlite`: SQLite database backend code.
* `src/jobu`: job scheduling and execution code.
* `src/jobuctl`: job control and command-line interface code.
* `src/jobud`: JobU daemon code.
* `src/net`: networking code.
* `src/rpc`: JSON-RPC code.
* `test/`: Catch2 test executables, one file per feature, named `*-test.cpp`.
* `.bld/`: local out-of-source build directory; do not commit generated build output.

## Stepwise Implementation Rule

For code changes, Codex must use a staged workflow.

* First propose a numbered plan before editing.
* Wait for approval before the first edit.
* Implement exactly one stage at a time.
* After each stage, stop and report changed files, validation, and the next proposed stage.
* Do not continue until the user explicitly approves.
* If new information invalidates the plan, stop and ask for approval of a revised plan.

## Build, Test, and Development Commands

* `cmake -B .bld -DCMAKE_BUILD_TYPE=Debug`: configure a Debug build.
* `cmake --build .bld`: build the `core` static library and all test executables.
* `ctest --test-dir .bld/test --output-on-failure`: run the registered Catch2 tests and show failures.
* `clang-format -i src/core/*.hpp src/core/*.cpp test/*.cpp`: format changed C++ files using the repository style.

The project requires CMake 3.20+, a C++20 compiler, `fmt`, `sqlite3`, `nlohmann_json`, and Catch2 discoverable via CMake package config.

## Clangd Diagnostics

When `.bld/compile_commands.json` is available, use the clangd MCP/LSP tools for C++ semantic exploration and diagnostics.

* The repository has an established diagnostics-clean baseline. For normal work, query diagnostics only for changed C++ source files and changed standalone C++ headers; do not repeat a project-wide scan unless the user explicitly requests one or there is evidence that the baseline is no longer valid.
* Diagnose a changed intentionally non-standalone or platform-specific header through an owning translation unit with the matching target configuration. Report unavailable target coverage instead of treating fallback-context parser errors as code defects.
* Treat every enabled compiler, include-cleaner, and clang-tidy diagnostic reported for those files as a completion blocker. Fix all diagnostics within the approved scope; do not silently accept or suppress them.
* If a fix would expand the approved stage, or a diagnostic conflicts with the intended design or project style, stop and report the exact location, check name, message, and rationale to the user.
* Preserve intentional indirect includes with `// IWYU pragma: keep <justification>`. Use `// IWYU pragma: export` when a public header deliberately re-exports another header, and remove genuinely unused includes. Do not add filename-based `.clangd` exceptions for include intent.
* Do not modify `.clangd` or add other local diagnostic suppressions without explicit approval.
* Re-run diagnostics for the changed files after fixes and report the stage complete only when those files are clean.

## Coding Style & Naming Conventions

Use the checked-in `.clang-format`. Important defaults are 4-space indentation, 120-column limit, left-aligned pointers, and custom brace wrapping with function braces on their own line.

Keep public API headers in `src/core` and implementation details in `.cpp` or `*_priv.hpp` files.

Follow existing naming patterns:

* snake_case file names such as `event_loop.cpp`;
* PascalCase types such as `EventLoop`;
* lowerCamelCase or descriptive method names as already used in nearby code.

Prefer small, focused classes and keep platform-specific code isolated behind backend files.

Match the style and abstraction level of nearby code unless there is a specific reason to improve it.

## Human-Readable Code Structure

Write code for human review and long-term maintenance, not for minimum token count, minimum line count, or maximum statement density.

A reader should be able to scan a non-trivial function and identify its major logical phases without parsing every statement.

* Separate logically distinct operations with blank lines.
* Treat blank lines as semantic punctuation. Do not remove them merely because the code remains syntactically valid without them.
* Keep closely related statements together.
* Start a new visual block when the operation, invariant, resource being manipulated, failure mode, or abstraction level changes.
* Avoid long uninterrupted walls of code.
* When a function contains several distinct phases, make those phases visually apparent through spacing, concise comments, helper functions, or a combination of these.
* Use short comments to identify non-obvious phases or explain rationale, ordering, invariants, ownership, or failure behavior.
* Do not add comments that merely restate individual statements or obvious mechanics.
* Extract a helper function when a block represents a meaningful independent concept and the extraction makes the caller easier to understand.
* Do not extract helpers merely to reduce function length or satisfy an arbitrary line-count target.
* Prefer straightforward control flow over compact expressions when the expanded form is materially easier for a human reviewer to understand.
* Avoid cleverness whose main benefit is fewer statements or fewer lines.
* Preserve useful intermediate variables when they make intent, lifetime, or error handling clearer.
* Prefer code whose structure communicates the algorithm before comments are needed to explain it.

As a rule of thumb, if more than roughly 10-15 consecutive lines form one uninterrupted logical block, consider whether a blank line, concise phase comment, or helper extraction would make the structure clearer. This is a readability prompt, not a hard line-count requirement.

## Exception Handling

Operational failures should normally be represented using return values such as `Result`, `bool`, or another API-specific error mechanism rather than exceptions.

Recoverable exceptions are part of the API contract only when explicitly documented.

* A function must not intentionally let a recoverable exception escape unless the exception is documented with Doxygen `@throws`.
* Do not add `@throws` merely because an implementation uses a standard-library operation that can theoretically throw.
* In particular, do not routinely document `std::bad_alloc` or exceptions arising solely from allocation failure or comparable unrecoverable resource exhaustion.
* Treat `std::bad_alloc` as a fatal process-level failure, not as part of the normal API error contract, unless a specific API explicitly requires different behavior.
* Never catch `std::bad_alloc`, including indirectly through `catch (...)` cleanup-and-rethrow handlers.
* Do not convert allocation failures into `Result` errors or attempt recovery from them; further allocations may fail in unrelated locations.
* Do not add `try`/`catch` blocks merely to make an API appear non-throwing when the only plausible exception is allocation failure or another unrecoverable implementation failure.
* An allocation failure reaching a `noexcept` boundary is allowed to terminate the process.
* Use `noexcept` to express an intentional non-throwing boundary or a semantic guarantee, not merely because ordinary operational failures are returned as values.
* Do not perform exhaustive transitive exception documentation of standard-library implementation details. Document exceptions that are meaningful parts of this project's API contract.

If a library operation can throw a recoverable exception that would otherwise cross a project API boundary, either convert it to the project's normal error representation or explicitly document the exception, according to the intended API design.

## Object Private Data and Signals

An `Object` subclass with private instance state must extend the single private block owned by `Object`.

Derive its private structure directly or transitively from `jb::core::priv::ObjectPrivate` and pass that one heap allocation to the protected `Object` constructor.

Do not add a second pimpl pointer or direct private implementation fields. Public `Signal` members and static process-wide state may remain direct class members.

Do not pass a usable derived owner into a private-data constructor before the `Object` base is constructed.

When private implementation code needs the public owner:

* bind the owner back-reference in the derived constructor body after `Object` has taken ownership of the private block;
* then install any connections that require the owner.

Use a signal for a reusable observable event emitted by an `Object`.

Use a receiver-aware connection whenever a slot captures an `Object`, so receiver destruction deactivates the slot.

Reserve context-free connections for callables that:

* borrow no `Object`;
* borrow only process-lifetime objects; or
* are explicitly disconnected before every captured target can be destroyed.

Keep callbacks for:

* one accepted operation's completion;
* strategies that produce a return value; and
* private non-`Object` adapter seams.

Treat useful public Doxygen and concise rationale comments at non-obvious ownership, lifetime, ordering, failure, and reentrancy boundaries as stage completion requirements.

## Documentation & Comments

Document public APIs with Doxygen-style comments.

Public API documentation should describe behavior that matters to callers:

* purpose and semantics;
* parameter requirements that are not obvious from the type;
* return-value meaning;
* ownership and lifetime requirements;
* important ordering or thread-safety rules;
* recoverable errors or documented exceptions that form part of the API contract.

Do not mechanically document implementation details that callers do not need to know.

In particular:

* do not add `@throws std::bad_alloc` merely because an implementation may allocate;
* do not list theoretical exceptions inherited transitively from standard-library calls unless they are intentionally part of the API contract;
* do not add verbose comments solely to satisfy a documentation requirement when the declaration is already self-explanatory.

In `.cpp` files, document multi-step function bodies when the implementation is not self-documenting.

Focus internal comments on intent, rationale, ordering, invariants, ownership, lifetime, and failure behavior.

Avoid comments that merely restate the function name or individual statements. No body comment is needed when the name and implementation already make the behavior clear.

Use visual structure as well as comments: comments should complement readable code organization, not compensate for a wall of code.

## Testing Guidelines

Tests use Catch2 with `Catch2::Catch2WithMain`.

Add new tests under `test/` as `feature-test.cpp`, register the executable in `test/CMakeLists.txt`, link it with `core` and Catch2, and add it with `add_test`.

Keep tests behavior-focused.

Prefer deterministic assertions and deterministic synchronization over timing-dependent sleeps.

Cover relevant event loop, timer, object lifetime, signal, threading, process, persistence, and error-handling behavior when those areas are changed.

Tests should validate externally meaningful behavior and important invariants rather than mirror implementation details unnecessarily.

## Scope Discipline

Stay within the approved implementation stage.

* Do not perform unrelated refactoring while implementing a requested change.
* Do not change public API, error semantics, ownership rules, threading behavior, or persistence format unless required by the approved plan.
* Do not "clean up" nearby code merely because it could be written differently.
* Small local readability improvements directly required to make changed code understandable are acceptable.
* If a desirable improvement materially expands scope, report it separately instead of silently including it.
* Preserve established behavior unless the approved plan explicitly changes it.

When existing code and a general coding preference conflict, preserve the existing design unless the task explicitly calls for changing it or the existing behavior is demonstrably incorrect.

## Commit & Pull Request Guidelines

Use a short subject that describes the behavior change; avoid vague subjects except for temporary local work.

The subject implicitly includes the prefix:

`This commit changes the software to`

The description should explain:

* rationale;
* important design decisions;
* relevant constraints;
* externally visible behavior changes, when applicable.

Do not commit yourself. Only suggest a commit message when asked.
