# JobU Phase 6 closure code-level design

Date: 2026-09-19  
Baseline: completed Stage 6.19 verification\
Proposed repository path: `docs/planning/jobu-phase6-closure-code-design.md`

## 1. Purpose and disposition

Close the single implementation deficit identified by the final Phase 6 audit, then update the recorded closure disposition before planning Phase 7. Implement this document in two individually reviewable stages, 6.20 and 6.21. Follow the current root AGENTS.md, including stopping after each stage for review and leaving commits to the user.

The confirmed defect is that CLI `output.capture=none` retains output in memory until completion. The associated documentation task is to replace obsolete provenance-recovery and pre-merge clean-tree requirements with the current decisions and accurately scoped verification results.

The user accepts the deleted Stage 6.17 macOS evidence and does not request recovery. Its absence is not a closure blocker. The separately reported Stage 6.18 native results remain historical evidence; they are not newly executed verification of this fix.

The previously rejected requirement for a synchronous all-descendants-terminal barrier remains rejected. Keep the agreed process-group signaling, direct-child reaping, and bounded output cleanup contract. No new process containment or supervision mechanism belongs to this work.

## 2. Evidence and defect mechanism

Baseline source references:

- `src/jobu/cli/cli_attempt_executor.cpp`: `ActiveAttempt` initializes `standard_output{stdout_limit}` and `standard_error{stderr_limit}` without considering `capture_mode_value`.
- The same file's `append_output()` appends every accepted chunk to these buffers, including mode `None`.
- `finish()` calls `take()` on both buffers before passing them to `map_cli_completion()`.
- `src/jobu/cli_capture_priv.cpp`: the buffer retains a prefix and suffix up to its limit; a zero limit already counts bytes without storing them.
- `src/jobu/cli_exit_policy_priv.cpp`: mode `None` reports zero captured bytes and omits `AttemptOutput`, hiding temporary retention from final-result assertions.
- `test/cli-attempt-executor-test.cpp`: existing `none` cases verify final metadata and absent output, but do not inspect retained bytes while the attempt is active.
- Phase 6 design section 17 explicitly requires `none` to drain and discard both streams.

With both configured limits at 64 MiB, a disabled-capture attempt can retain 128 MiB of payload, with additional allocation while `take()` assembles the completion snapshot. Retention and copying multiply across concurrent attempts. Correct final JSON does not make that runtime behavior conformant.

The final audit independently compared the tested Stage 6.18 source with the baseline: only README/planning documents changed. The handoff reports Linux 114/114 and carried native macOS 115/115 CTest targets passing. These counts describe historical runs, not this closure patch. The audit workspace lacked CMake, so it did not independently repeat those runs.

## 3. Required behavior

| Mode | Retention while active | Byte accounting | Durable output |
| --- | --- | --- | --- |
| `none` | Zero retained payload bytes in both capture buffers, regardless of configured limits | Continue counting all successfully observed bytes | No `AttemptOutput` |
| `on_error` | Existing configured prefix/suffix retention | Unchanged | Persist on failure/cancellation; omit on success |
| `always` | Existing configured prefix/suffix retention | Unchanged | Persist for every supported outcome |

For `none`, preserve the existing result contract: `captured_bytes=0`, exact `total_bytes`, and `truncated=(total_bytes>0)`. Empty streams report zero counts and false truncation. Preserve `capture_lost` when Process reports a lost channel or byte accounting fails; disabling capture does not hide observation failures.

Transient Process chunks still exist while signals deliver them. This requirement prohibits cumulative retained output; it does not claim zero allocations throughout process execution. Keep draining after configured limits are exceeded, with unchanged timeout/cancellation responsiveness and exactly-once completion.

Do not change public APIs, schema v1, API version, attributes/defaults, retry classification, result JSON shapes, output persistence rules, scheduler capacity accounting, or Process lifecycle. Do not impose a new zero-limit validation rule on users choosing `none`.

## 4. Production implementation

### 4.1 Select the effective limits once

In `CliAttemptExecutor::Private::ActiveAttempt`, initialize the capture buffers with zero limits when the supplied mode is `CliCaptureMode::None`. Use `capture_mode_value`, which is already available as a constructor argument:

```cpp
, capture_mode{capture_mode_value}
, standard_output{capture_mode_value == jb::jobu::detail::CliCaptureMode::None ? 0U : stdout_limit}
, standard_error{capture_mode_value == jb::jobu::detail::CliCaptureMode::None ? 0U : stderr_limit}
```

Format to the repository style. Add one concise rationale comment explaining that zero retention preserves stream accounting without storing disabled-capture payloads. Keep the configured policy values intact elsewhere; only the active buffers' effective limits change.

Continue using `append()` and `take()`. Zero-limit buffers already provide the required accounting and empty output snapshots. An early return from `append_output()` for mode `None` is incorrect because it would lose total-byte and overflow accounting. Do not introduce a second counter implementation or modify `map_cli_completion()` to mask this defect further.

### 4.2 Preserve ownership and event boundaries

Keep all executor instance state inside its Object-owned private block. Add no second pimpl or direct public-class member. Preserve receiver-aware Process signal connections and the private non-Object adapter callbacks. No new signal is necessary for test observation.

Keep existing failure semantics: unrecoverable allocation failures are not converted to normal errors; add no catch-all handlers or routine `@throws std::bad_alloc` documentation.

## 5. Deterministic regression observation

The regression must fail with the baseline constructor and pass with zero effective limits. Repeating final JSON assertions alone is insufficient.

Extend the existing private test access, without changing `cli_attempt_executor.hpp`:

1. Add an inexpensive `retained_size() const noexcept -> std::size_t` observer to `CliCaptureBuffer` in `src/jobu/cli_capture_priv.hpp`. Return `_prefix.size() + _suffix.size()`. This header is private despite the method being accessible to internal callers. Do not assemble or copy a buffer to inspect it.
2. In `src/jobu/cli/process_adapter_priv.hpp`, define a small `CliRetainedOutputSizes` value with `stdout_bytes` and `stderr_bytes` fields and add the following method to the existing `CliAttemptExecutorTestAccess`:

```cpp
[[nodiscard]] static auto retained_output_sizes(
    CliAttemptExecutor const& executor, AttemptKey const& key)
    -> std::optional<CliRetainedOutputSizes>;
```

3. Implement it in `cli_attempt_executor.cpp`, where `Private` is complete. Use the existing friend access, look up `active_by_attempt`, return `std::nullopt` for an inactive key, and otherwise return both buffers' retained sizes. Query on the owner thread. Include `<cstddef>` and `<optional>` directly where required.

The probe returns values only, does not modify the attempt, and exposes no buffer pointers, contents, or production observer API. Document its private purpose, owner-thread rule, and inactive-key result concisely. No timing sleeps, process RSS measurements, global allocation hooks, or new dependencies are needed.

## 6. Test specification

### 6.1 Active executor regression

Use the existing fake Process adapter and executor fixture in `test/cli-attempt-executor-test.cpp`:

- Materialize `output.capture=none` with valid nonzero stdout/stderr limits. Use distinct small limits such as 7 and 5; the defect does not require megabytes of test data.
- Start an accepted attempt and query zero retained sizes before output.
- Deliver multiple stdout and stderr chunks through the existing fake adapter, including a chunk larger than each configured limit and binary bytes. After every chunk, assert both retained sizes remain zero and completion has not occurred.
- Finish the attempt. Assert exactly one completion, no `AttemptOutput`, zero captured bytes, exact independent stream totals, and true truncation for nonempty streams. The private query now returns `nullopt`.
- Exercise an empty-stream completion to retain zero/false metadata semantics.
- Exercise a `none` completion with Process capture loss and verify `capture_lost=true` without a durable output row. Reuse existing coverage where it already proves this behavior.
- Include a control using `always` or `on_error` with nonzero limits: the observer must report positive retained sizes bounded by those limits. This demonstrates that the probe actually observes buffers and that the fix did not disable other capture modes.

Reuse existing expected success/failure/cancellation mapping tests instead of generating a full Cartesian product. The retention choice is made once and is independent of eventual exit classification.

### 6.2 Buffer behavior and accounting

Existing zero-limit buffer tests already cover single-chunk discard. Extend `test/cli-capture-test.cpp` with a multi-chunk zero-limit case that checks `retained_size()==0` after each append, exact totals after `take()`, truncation for nonempty input, and reset/reuse behavior. Keep existing first/last ordering and checked-overflow tests.

Do not simulate uint64 overflow by sending enormous streams. The existing pure checked-total helper tests cover the arithmetic boundary; executor loss handling remains unchanged.

### 6.3 Validation commands

Use the implementation checkout's configured tools and build directory. Example Linux commands:

```sh
cmake -S . -B .bld -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld --target cli-capture-test cli-attempt-executor-test cli-exit-policy-test -j 4
ctest --test-dir .bld/test --output-on-failure \
  -R '^(cli-capture-test|cli-attempt-executor-test|cli-exit-policy-test)$'
```

Confirm actual target names from `test/CMakeLists.txt` before running. Demonstrate the new active-retention regression failing against the old constructor in a disposable comparison, then passing with the fix. Record the assertion failure; do not leave the implementation checkout reverted.

Query configured clangd diagnostics for changed C++ files. Diagnose private headers through their owning translation units when required. Preserve the repository's configured flags; add no unrelated `-Werror` gate, suppression, or standalone-private-header requirement.

No new SQLite-disabled build or duplicate full test run is required: target boundaries and dependencies are unchanged, and the previous compile-only result remains historical evidence. Root execution and platform process-supervision code are unchanged; do not repeat privileged root tests for this correction.

## 7. Stage 6.20 — Fix disabled capture and prove active discard

**Scope:** the two constructor initializers, concise rationale, private retention observers, and focused regression tests described above.

Expected files:

- `src/jobu/cli/cli_attempt_executor.cpp`
- `src/jobu/cli/process_adapter_priv.hpp`
- `src/jobu/cli_capture_priv.hpp`
- `test/cli-attempt-executor-test.cpp`
- `test/cli-capture-test.cpp`

Implementation sequence:

1. Confirm the checked-out revision and that the defect still exists; preserve unrelated changes.
2. Add the private observation path and deterministic regression, establishing failure on the old buffering behavior.
3. Select zero effective retention limits for mode `None`.
4. Run the focused tests and changed-file diagnostics; review the diff for accidental public API or policy changes.
5. Report exact source revision/working-tree delta, commands, outcomes, and any unavailable validation.

**Exit criteria:** zero retained bytes while active under `none`; exact stream accounting and capture-loss metadata; unchanged other modes; the regression fails before and passes after the fix; changed-file diagnostics are clean.

Suggested commit subject: `discard disabled CLI capture while preserving byte counts`

Stop for review before Stage 6.21.

## 8. Stage 6.21 — Record verification and close Phase 6

**Scope:** verify the integrated correction and update the existing closure records. This stage is not authorization to implement Phase 7.

Build the normal SQLite-enabled Linux configuration and run the full registered suite once:

```sh
cmake --build .bld -j 4
ctest --test-dir .bld/test --output-on-failure
```

A fresh build directory is optional for this localized correction. Record whether it was clean or incremental, actual test counts and skips, and the exact tested source/diff. If Stage 6.20 already supplied a full passing run at identical source, reuse it instead of repeating it solely for this stage.

If native macOS is available, run the same focused three targets against the closure patch and record the revision. This is new evidence, not recovery of deleted Stage 6.17 records. It is not a closure prerequisite for this platform-neutral correction. Clearly state if no new native run occurred.

Update these tracked documents together:

| Document | Required update |
| --- | --- |
| `README.md` | Remove the obsolete claim that deleted native provenance must be recovered. Link the closure design/verification record and state the actual closure status. |
| `docs/planning/README.md` | Add this closure design, record stages 6.20/6.21 and their actual status, and remove obsolete recovery gates. |
| `docs/planning/jobu-phase6-code-design.md` | Preserve historical stage definitions; append a concise final-audit/closure disposition linking this design and the verification addendum. Replace obsolete current-status statements without rewriting prior test history. |
| Shared phase verification record | Record closure-patch verification and update the disposition. Preserve original measurements, tested revisions, and platform limitations in the shared evidence document, outside repository planning files. |

The verification addendum must record:

- the defect, zero-retention implementation, and active-state regression;
- the tested closure revision or exact working-tree patch identity;
- new commands/results, with historical 6.19 and carried 6.18 results labeled separately;
- the user's acceptance of deleted Stage 6.17 evidence, with no pending recovery task;
- that the 6.19 merge clean-tree comparison was completed, distinct from the new closure patch's merge status;
- remaining Phase 7 recovery/shutdown and Phase 8 API/secrets boundaries.

During implementation, say "closure fix verified; merge pending" when that is the actual state. After the user's merge, compare merged source/tests/build configuration with the tested closure tree and record the final revision. A documentation-only delta needs no repeat full suite. Do not describe an unperformed post-merge comparison as completed or reopen the already satisfied 6.19 gate.

**Exit criteria:** integrated Linux validation passes; the implementation finding is resolved; closure notes reflect the user's evidence decision; the final disposition is accurate for the current merge state. Phase 6 may be declared closed when the merged source matches the verified correction, enabling Phase 7 planning.

Suggested commit subject: `record Phase 6 capture fix and closure disposition`

## 9. Final review checklist

- [ ] Mode `none` retains no output payload in either active capture buffer.
- [ ] Counting, overflow detection, capture loss, truncation metadata, and exactly-once completion remain intact.
- [ ] Other capture modes preserve configured retention and persistence behavior.
- [ ] The active-state regression detects the original defect; final JSON checks alone are not the proof.
- [ ] Object private-data ownership, signals, callbacks, public APIs, and schema are unchanged.
- [ ] Focused tests, integrated Linux validation, and changed-file diagnostics are recorded accurately.
- [ ] No deleted macOS evidence recovery is required or claimed.
- [ ] Historical and new test results remain distinguishable.
- [ ] Documentation reflects actual verification/merge status and the accepted scope boundaries.
- [ ] No all-descendants observation barrier or Phase 7/8 implementation was introduced.
