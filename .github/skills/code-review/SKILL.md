---
name: code-review
description: Review Deferra pull requests for concrete defects while respecting the approved JobU phase and stage boundaries in docs/planning. Use for every GitHub pull request code review in this repository.
---

# Objective

Find concrete defects introduced by a pull request without requesting work that
the approved plan assigns to a later phase or stage.

Use `AGENTS.md` for repository-wide coding, diagnostics, documentation, and
testing standards. Do not duplicate those instructions here.

# Establish the review scope

1. Read the pull request title and description.
2. Identify the declared planning document and implementation stage.
3. Read `docs/planning/README.md`.
4. Open the referenced planning document.
5. Read:
   - the current stage section;
   - its `Implement`, `Verification`, and `Exit` requirements;
   - only the design sections referenced by that stage;
   - applicable explicit exclusions or deferred entry boundaries.
6. Inspect the changed implementation and directly affected callers, callees,
   public headers, CMake targets, and tests.

Do not load an entire long planning document when the current stage and its
referenced sections provide sufficient context.

If the pull request does not identify a plan and stage, do not guess its planned
scope. Review the behavior implemented by the pull request against existing
public contracts and repository conventions.

# Respect staged delivery

Do not report missing behavior when the approved plan explicitly assigns that
behavior to a later stage or phase.

Do not request adjacent-stage implementation for completeness.

A planned deferral does not suppress a finding when the current change:

- violates a current-stage requirement;
- is incorrect or unsafe in its current form;
- breaks an already merged public contract;
- introduces a security, lifetime, concurrency, data-integrity, or portability
  defect;
- creates externally observable behavior that the current stage forbids;
- makes the documented later implementation impossible without undoing the
  current design.

If the pull request changes its referenced plan, `AGENTS.md`, or this skill,
call that review-context change to human attention. Do not use newly weakened or
newly deferred requirements to dismiss an otherwise valid finding without
human confirmation.

# Review priorities

Prioritize:

- ownership and object lifetime;
- asynchronous admission, cancellation, completion, and exact-once behavior;
- callback reentrancy and EventLoop registration/cleanup ordering;
- transaction atomicity, rollback, durable-before-side-effect ordering, and
  fail-closed persisted-data handling;
- public API compatibility, Doxygen, and first-include header boundaries;
- private dependency isolation, including SQLite, libcurl, and JSON types;
- sensitive-data exposure through errors, diagnostics, or logs;
- Linux, macOS, epoll, kqueue, and musl portability where relevant;
- deterministic behavior-focused tests and applicable CMake configurations.

For changes crossing optional SQLite boundaries, check that
`JB_BUILD_SQLITE_DRIVER=OFF` remains viable.

For HTTP changes, verify that curl types and curl compile requirements remain
private to the system HTTP backend.

# Finding quality

Report a finding only when you can state:

1. the concrete triggering condition;
2. the incorrect observable behavior;
3. the violated current contract or invariant.

Anchor the comment to the narrowest relevant changed line. Keep one issue per
comment and suggest the smallest suitable correction.

Do not report:

- speculative future concerns without a concrete current failure;
- style-only issues handled by clang-format or enabled diagnostics;
- unrelated refactoring opportunities;
- missing functionality explicitly assigned to a later stage;
- requests to broaden the pull request beyond its approved boundary.

Prefer no comment over a low-confidence or unactionable comment.
