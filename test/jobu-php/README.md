# PHP API integration tests

Select an explicit `PHP_CONFIG` and keep CLI, headers, module and optional Embed
library from that installation. The compatibility matrix runs PHP 8.2–8.6 NTS
on the host, Ubuntu 26.04 and Alpine 3.24. `tools/ci/php-test` builds only
PHP-dependent sources against the matching native SDK and daemon, then rejects
missing or skipped required checks. PHP 8.1 remains the minimum contract but is
not a tested matrix row; JobU ZTS is unsupported.

## Contract and behavior coverage

`reflection-contract-test.cmake` reflects the authoritative
`src/jobu-php/jobu.stub.php` without the extension, then reflects `jobu.so` in a
separate process. It compares every declared method, including private
constructors, parameter names/types/defaults/attributes, return types, visibility,
inheritance, enum cases and class inventory. Union members are order-independent.
Native enum registration omits the userland final bit; isolated child processes
verify that inheritance is rejected before treating both representations as
non-inheritable. No parser, generated-header freshness gate, PHP source checkout
or `gen_stub.php` is needed. Failure snapshots are written to the test build
directory as `reflection-expected.json` and `reflection-actual.json`.

The behavior suites remain separate from signature equality. The following map
covers all public owners; method-by-method review must account for dynamic calls
such as the typed getter/setter table in `attributes.php`, rather than searching
only for literal method calls. The native operation map is in
[`docs/php.md`](../../docs/php.md#control-api-coverage-audit).

| Public owner | Behavior assertions | Execution |
| --- | --- | --- |
| All enums | `reflection.php`: cases, values, conversion, native serialization | Production CLI |
| Exception hierarchy | `exceptions.php`, bridge/recovery cases: metadata and translation | CLI and Embed |
| Duration, SecretReference, Schedule, OnceSchedule, CronSchedule | `values.php`, lifecycle adapters, `secrets-schedules.php` | CLI and Embed |
| JobAttributes, CliJobAttributes, HttpJobAttributes, QueueDefaults | `attributes.php`, queue/job configuration and adapter cases | CLI and Embed |
| CliPayload | `cli-payload.php`, coercion and lifecycle adapter cases, `job.php` | CLI and Embed |
| HttpPayload, HttpHeader | `http-payload.php`, coercion and lifecycle adapter cases, `job.php` | CLI and Embed |
| TimeRange, RunQuery, StatisticsQuery | `queries.php`, history/statistics filters and adapters | CLI and Embed |
| Client | `client.php`, `queue.php`, list/history/secrets/schedules/statistics scenarios; bridge/recovery cases | CLI and Embed |
| Queue, Job | `queue.php`, `job.php`, scoped lists/history/statistics | CLI and Embed |
| Page | `list.php`, `history.php`, `statistics.php`; `page.php` and cursor/proxy cases | CLI and Embed |
| Run, Attempt, RunSummary, AttemptSummary | `history.php`, `output.php`, scoped lookup/proxy cases | CLI and Embed |
| ExecutionResult, CliExecutionResult, HttpExecutionResult, OutputChunk | Real executions in history/output; lifecycle codec projections and numeric/binary boundaries | CLI and Embed |
| SystemInfo, SecretMetadata | `statistics.php`, `secrets-schedules.php`, lifecycle metadata projections | CLI and Embed |
| StatisticsGroup, StatisticsRunCounts, StatisticsAttemptCounts, StatisticsCaptureCounts, StatisticsDuration, StatisticsRunnableWaitCoverage, StatisticsMetadata, StatisticsMeasurement | `statistics.php`, `statistics-test.cpp`: typed counters, windows, grouping, metadata, empty and error cases | CLI and Embed |

## Production CLI and cleanup

`cli-test.cpp` links the native Client SDK and Catch2, without Zend or Embed.
It starts a same-build `jobud` using `daemon-fixture.hpp`, a temporary SQLite
database and Unix socket, waits for readiness, and launches the matching PHP CLI
with `-n` and the production module. Each scenario gets isolated daemon state.
The queue/job wrapper reuses the same assertions as the Embed tests. Existing
list, history, output, secret/schedule and statistics scripts are also reused.
Both stdout and stderr diagnostics and child exit status are checked.

`restart-test.cmake` repeats a real connected CLI load/use/shutdown three times.
`connected-lifecycle.php` checks execution results, explicit close, independent
clients, attached-owner retention, release through GC, detached snapshots and
clone/serialization policy. It deliberately leaves an open resource graph for
shutdown, including Pages, summaries, attached configuration and output/results.

The Embed driver runs that same production-only graph through three requests
in each of three complete runtimes. It checks fresh request state, restored host
event-loop ownership and, on Linux, descriptor counts after request and module
shutdown. The existing fixture-only lifecycle and per-feature cases remain.
Separate CLI processes and reused Embed requests do not establish real FPM
coverage or arbitrary Zend bailout recovery.

Without Embed, CLI, restart and Reflection checks still run; the Embed checks
explicitly skip. Without a matching daemon, daemon-dependent checks explicitly
skip. CI promises both dependencies and rejects either situation. Alpine root
execution requires the existing `JOBU_TEST_ALLOW_ROOT_CLI=1` opt-in; daemon root
permission alone never authorizes CLI jobs.

For full validation, exclude the `distribution` label as required by the project
workflow. Build/test native code once per environment and reuse its matching SDK
across ABI rows. Record the exact source hashes, interpreter/build modes, SDK
provenance, commands and JUnit counts. Sanitizer results must state which units
were instrumented; imported SDKs and installed PHP are not implicitly covered.
