# JobU PHP extension development

This directory implements `jobu.so`. PHP application developers should use the [PHP API
manual](../../docs/php.md). Build, install, enable, and SDK reuse commands are in the
[root README](../../README.md#php-builds-and-installation). All shell commands below run
from the repository root.

## Module linkage

The SDK must match the source revision, distribution, architecture, compiler, and system
libraries. Its `Client` component provides PIC static libraries and requires fmt; it
does not import the SQLite backend or daemon. The module exports the PHP loader entry
point and hides its own and linked archive implementation symbols. Production linkage
excludes daemon/runtime, database backend and Embed implementation. Shared domain codecs
from the Client SDK remain part of the module.

## Request and session ownership

Registration happens during MINIT. Zend owns the registered classes and exception
properties. RINIT creates a private request owner; its event loop is attached lazily
when the first native session is created. Each session owns a separate socket, raw RPC
client, and typed client. The request registry borrows sessions and holds no Zend
references. All native and Zend access stays on the PHP calling thread; there is no
worker thread or process-wide connection cache.

RSHUTDOWN clears the active request pointer, closes and detaches every remaining
session, then releases the loop and restores the previous thread binding. Session close
first removes callback receivers, then destroys typed/raw clients and the socket.
Cleanup is synchronous and does not dispatch pending work. Later native wrapper
destruction is safe even when Zend frees object storage after RSHUTDOWN. Explicit close
is idempotent and affects only that session. The loop stays attached until request
shutdown, even after all sessions close.

## Synchronous bridge and recovery

The private bridge wraps typed `ControlClient` calls, preserving owning replies,
local/remote failures, and mutation uncertainty. A terminal notification does not
replace its subsequent per-call outcome. Timeout or stopped-wait cleanup retires local
observation only; it does not cancel remote work or prove rollback. A terminal failure
already queued but not delivered at expiry is conservatively uncertain. The bridge
implements reconnect/retry and operational exception translation;
`Client::getSystemInfo()` uses the same typed read path. There is no public raw RPC
entry point. Test-only bindings exercise calls without adding placeholder APIs. No C++
exception may unwind into Zend. Allocation failure remains fatal, and PHP bailout does
not guarantee C++ stack cleanup.

Public management and resource methods use the private typed bridge for recovery. Each
operation owns frozen request parameters. Replacement connections repeat the
API/capability handshake and replace the whole socket/raw/typed-client bundle. They
never revive a terminal native client. All attempts, reconnect delays, and internal
reads share the original monotonic deadline. A per-call Duration override does not
change the client's default. Reconnect waits use bounded event-loop timers.

Creation and Run Now receive an automatically generated, OS-random key unless one was
supplied. The same key survives every in-call retry. Automatic keys combine two freshly
seeded UUIDv7 values (148 random bits, 72 UTF-8 bytes), using the existing SDK
generator. Explicit keys remain unchanged; the bridge validates the 1–128 UTF-8 byte
contract before sending.

## Private resource-state foundation

The production Queue and Job bindings share internal cache/dirty-state machinery. The
fixture-only resource used by Embed tests is not registered by `jobu.so`.

`ResourceState<Queue>` and `ResourceState<JobDefinition>` retain the originating PHP
Client and keep independent accepted and working native snapshots. The concrete binding
traces its attached configuration objects as well as the state's Client edge. No
Client-to-resource ownership edge is added. All state and Zend access remains on the
originating request/thread; use the default non-cloneable, non-serializable
`ObjectBinding` policy for connected resources.

Setters track net differences from the accepted snapshot. Reverting a field removes its
edit; job attributes track changed keys and produce a supplied-key patch, whereas queue
defaults and payloads use full replacement. Intermediate attribute edits may be
temporarily inconsistent across fields, so dirty tracking copies native values without
whole-layer validation. Request construction validates the complete configuration before
freezing a typed operation.

A clean save returns without RPC. Dirty refresh and lifecycle guards fail locally with
`UnsavedChangesException`. Save freezes the stable UUID, accepted revision and changed
fields, and uses the existing operation/recovery bridge with one total deadline. A
validated successful reply becomes the new baseline; conflicts, uncertain outcomes and
conversion failures preserve all local edits and the old baseline/revision. There is no
added replay or reconciliation policy. Discard restores the last accepted snapshot
locally, even after Client closure. RPC operations continue to use the existing Client
boundary.

Attached attributes/defaults and CLI/HTTP payloads provide private prepared restoration
adapters. Prepare every component before committing any component; commit moves native
values into the existing objects/backing layers, without notifications, PHP callbacks or
GC. The temporary restoration borrows its target, so it must not survive a PHP call or
its target. Payload restoration accepts an already converted configuration source; the
concrete Job binding decodes Job replies. Ordinary edits preserve their existing
notifications, merge/replacement semantics and detached clone behavior.

An optional pre-commit edit guard lets an attached child reject edits after its resource
is locally deleted. Setters recheck after conversions that can execute PHP. Clones
detach both the owner and its guard/notification. Local deletion retains cached getters
and the last received revision; a null delete reply does not fabricate a new token.
Explicit historical refresh can use the shared cache path where the concrete resource
API supports it.

## Stub maintenance and validation

`src/jobu-php/jobu.stub.php` is authoritative. Generate and commit `jobu_arginfo.h` when
changing that surface, using PHP's `build/gen_stub.php` and its matching PHP-Parser
installation. The stub requests class entries and PHP 8.1-compatible registration
through `@generate-legacy-arginfo 80100`:

```sh
/path/to/php-8.5/bin/php -n -dextension=tokenizer \
    /path/to/php-8.5-source/build/gen_stub.php src/jobu-php/jobu.stub.php
```

Generation is a maintainer operation. Normal CMake builds and CI consume the committed
header without PHP sources, a generator, or a freshness check. Diagnose the generated
header through `module.cpp` with the selected ABI, and format only handwritten C++.

CTest runs configuration rejection, module listing/information, Reflection and enum
contracts, exception behavior, and repeated child-process load/shutdown checks. An
additional Embed regression runs the production module hooks through three runtime
lifetimes with five requests each. It checks two connected sessions,
close-one/use-other, strong resource-to-client ownership, GC cycles, partial
construction, pending work, late native wrapper destruction, and reentrancy rejection.
Linux also checks descriptor accounting. This models reused PHP-FPM request boundaries;
it is not a claim of testing an actual PHP-FPM deployment.

Bridge tests additionally cover immediate/deferred replies, capability/version
rejection, constructor errors, one remaining deadline, pre-acceptance and remote
failures, timeout/stopped-wait cleanup, late replies, terminal mutation uncertainty, and
reentrancy. Recovery tests drop successful real-daemon replies before forwarding them,
compare replay parameters and keys, and assert committed resource identity and
single-transmission saves. They also cover restart, incompatible replacement peers,
terminal deadlines, next-call recovery, per-call overrides, exception
metadata/redaction, and independent clients. The real module is also loaded by a
separate PHP CLI process against the same-build daemon. Reused-runtime tests exercise
production Client construction, independent sessions, Duration conversions, and request
teardown.

The fixture classes and their generated arginfo are linked only into the test
executable. The production module exports no test API and does not link Embed.
Integrated builds use the `jobud` target. Standalone tests take `JOBU_PHP_TEST_DAEMON`;
missing Embed or daemon produces explicit skips for lifecycle/bridge checks, which the
CI gate rejects. The lifecycle regression covers normal request shutdown, not arbitrary
recovery after Zend bailout or a process kill. Standalone module tests also run the
independent helper suite, including relocated helper consumers.

The Ubuntu 26.04 and Alpine 3.24 PHP CI rows consume their native job's SDK from **the
same workflow run**. The archive also contains `fixtures/jobud`, staged from that native
build outside the installed Client SDK. PHP rows install the existing daemon's runtime
library dependencies and pass its path to the lifecycle test. Each row rebuilds only
helper/module/test sources. The single `&php-versions` list in
`.github/workflows/ci.yml` controls both matrices. After extracting the matching
archive, reproduce a row with:

```sh
CCACHE_DIR="$PWD/.ccache" tools/ci/php-test 8.5 \
    /usr/bin/php-config8.5-zend .bld/php /path/to/sdk
```

On Alpine use `/usr/bin/php-config85zend`. Omitting the SDK argument retains helper-only
validation. Both modes reject missing or skipped required checks.

## Packaging validation

Packaging regressions stage and load the installed INI/module, check component isolation
and destination overrides, and inspect Linux dynamic dependencies and selected static
archive members. The helper's relocated consumer checks remain independent. CI requires
these checks in every NTS row and rejects native source recompilation in standalone PHP
builds.

## Control API coverage audit

The current daemon registers 30 capabilities. The statistics integration scenario checks
the full advertised inventory, and the following mapping records the public PHP owner
and behavior tests. Test paths below are relative to `test/jobu-php/`. The mapping
deliberately keeps queue/job/run/attempt ownership in the public API.

| Capability | Typed ControlClient method | Public PHP owner/method | Behavior tests |
| --- | --- | --- | --- |
| `system.info` | `get_system_info` | `Client::getSystemInfo`; constructor handshake | `statistics.php`, `statistics-test.cpp`, `bridge-test.cpp` |
| `system.stats` | `system_statistics` | `Client::systemStatistics` | `statistics.php`, `statistics-test.cpp` |
| `queue.create` | `create_queue` | `Client::createQueue` | `queue.php`, `queue-test.cpp` |
| `queue.get` | `get_queue` (name and UUID overloads) | `Client::getQueue/getQueueById`, `Queue::refresh` | `queue.php`, `queue-test.cpp` |
| `queue.list` | `list_queues` | `Client::listQueues` | `list.php`, `list-test.cpp` |
| `queue.update` | `update_queue` | `Queue::save` | `queue.php`, `queue-test.cpp` |
| `queue.suspend` | `suspend_queue` | `Queue::suspend` | `queue.php`, `queue-test.cpp` |
| `queue.resume` | `resume_queue` | `Queue::resume` | `queue.php`, `queue-test.cpp` |
| `queue.delete` | `delete_queue` | `Queue::delete` | `queue.php`, `queue-test.cpp` |
| `queue.stats` | `queue_statistics` | `Queue::statistics` | `statistics.php`, `statistics-test.cpp` |
| `job.create` | `create_job` | `Queue::createCliJob/createHttpJob` | `job.php`, `lifecycle-test.cpp` |
| `job.get` | `get_job` | `Queue::getJob`, `Job::refresh` | `job.php`, `history.php`, `history-test.cpp` |
| `job.list` | `list_jobs` | `Client::listJobs`, `Queue::listJobs` | `list.php`, `list-test.cpp` |
| `job.update` | `update_job` | `Job::save` | `job.php`, `lifecycle-test.cpp` |
| `job.suspend` | `suspend_job` | `Job::suspend` | `job.php`, `lifecycle-test.cpp` |
| `job.resume` | `resume_job` | `Job::resume` | `job.php`, `lifecycle-test.cpp` |
| `job.move` | `move_job` | `Job::move` | `job.php`, `history.php`, `history-test.cpp` |
| `job.delete` | `delete_job` | `Job::delete` | `job.php`, `history.php` |
| `job.run_now` | `run_now` | `Job::runNow` | `job.php`, `lifecycle-test.cpp` |
| `run.get` | `get_run` | `Job::getRun`, `RunSummary::load`, `Run::refresh` | `history.php`, `history-test.cpp` |
| `run.list` | `list_runs` | `Client/Queue/Job::listRuns` | `history.php`, `history-test.cpp` |
| `run.cancel` | `cancel_run` | `Run::cancel` | `history.php`, `history-test.cpp` |
| `attempt.get` | `get_attempt` | `Run::getAttempt`, `AttemptSummary::load`, `Attempt::refresh` | `history.php`, `history-test.cpp` |
| `attempt.list` | `list_attempts` | `Run::listAttempts`; latest-attempt selection for output | `history.php`, `history-test.cpp`, `output-test.cpp` |
| `attempt.output` | `read_attempt_output` | `Attempt::readOutput/getOutput`, `Run::getOutput` | `output.php`, `output-test.cpp` |
| `secret.set` | `set_secret` | `Client::setSecret` | `secrets-schedules.php`, `secrets-schedules-test.cpp` |
| `secret.list` | `list_secrets` | `Client::listSecrets` | `secrets-schedules.php`, `secrets-schedules-test.cpp` |
| `secret.delete` | `delete_secret` | `Client::deleteSecret` | `secrets-schedules.php`, `secrets-schedules-test.cpp` |
| `schedule.validate` | `validate_schedule` | `Client::validateSchedule` | `secrets-schedules.php`, `secrets-schedules-test.cpp` |
| `schedule.next` | `next_schedule_occurrences` | `Client::nextScheduleOccurrences` | `secrets-schedules.php`, `secrets-schedules-test.cpp` |

The remaining native methods are lifecycle machinery: `initialize()` drives the
constructor/reconnect handshake; `cancel_call()` retires timed-out bridge calls;
`close()` backs explicit Client closure and request shutdown. `bridge-test.cpp` and
`lifecycle-test.cpp` cover acceptance, cancellation, deferred replies, deadlines,
recovery and teardown. These need no protocol-level PHP escape hatch.

Statistics PHP tests cover typed projection, windows, scoping, cursor-only requests,
real eviction/restart, safe-read replay, total deadlines, metadata atomicity and
close/request teardown. Existing native `statistics-service-test.cpp` supplies
clock-controlled five-minute expiry, retained retries outside the cohort window,
retention and measurement correctness. `jobud-runtime-test.cpp` checks configured
statistics response budgets; the PHP proxy checks propagation of those errors.
Reflection checks cover the public method/default/type contract. See the [PHP test
README](../../test/jobu-php/README.md) for integration, reliability, and packaging
regression details.

## Reusing the native SDK in CI

After each native job passes, `tools/ci/prepare-sdk` installs the existing `Development`
component, relocates it, builds/runs the independent Client SDK consumer, and packages
`jobu-sdk.tar.gz`. It recompiles no JobU library. The workflow publishes
`jobu-sdk-ubuntu-26.04-x64` and `jobu-sdk-alpine-3.24-x64` as artifacts of that workflow
run; the archives contain the `sdk/` prefix and its PIC static libraries, public
headers, and CMake exports.

The module's standalone build uses `find_package(JobU CONFIG REQUIRED COMPONENTS
Client)` and links `JobU::jobu-client`, while building `php-cxx` and the module for the
selected PHP ABI. Each PHP matrix depends on its corresponding native job, downloads the
SDK artifact **from the same workflow run**, extracts it, and passes the relocated
`sdk/` prefix through `CMAKE_PREFIX_PATH`. The consumer uses the matching distribution,
architecture, compiler, and system-library environment. Never substitute a cached SDK
from a different revision, mix glibc and musl artifacts, or configure the top-level
project in each PHP row.

```sh
CCACHE_DIR="$PWD/.ccache" tools/ci/php-test 8.5 /usr/bin/php-config8.5-zend \
    .bld/php /path/to/relocated/sdk
```

Configuration, module load, Reflection, exception, process restart, daemon integration,
and packaging checks run alongside all twelve helper checks. CI rejects missing or
skipped checks in either suite and uploads the compilation database to show which
sources were rebuilt. Daemon integration reuses the separately staged matching
`fixtures/jobud`. The standalone helper remains independent of the SDK; its original
three-argument test command and `src/php-cxx` entry point remain available.
