# JobU Phase 5 Closure Code-Level Design

**Status:** implementation-ready design  
**Baseline:** GitHub `main` at `447e8b0e0713e026c32c28c31524e6c1b56c49c4`  
**Stage range:** 5.20 through 5.32  
**Repository path when adopted:** `docs/planning/jobu-phase5-closure-code-design.md`

## 1. Purpose

Phase 5 already satisfies its functional HTTP-runner exit condition. This closure work resolves the architectural
deficits found during the final review before Phase 6 introduces the process abstraction and CLI runner.

The closure has two goals:

1. every existing `jb::core::Object` subclass stores implementation state in the single private block owned by
   `Object`, rather than in direct instance fields or a second `std::unique_ptr<Private>`; and
2. reusable event notifications use the signal/slot mechanism, while callbacks remain only where their one-shot,
   return-value, or non-`Object` semantics make a signal inappropriate.

The functional Phase 5 contract remains unchanged: HTTP scheduling, output persistence, retry classification, TLS and
redirect policy, daemon startup, management RPC method names, and the database schema must behave exactly as they do
at the baseline revision.

## 2. Exit condition

Phase 5 is closed when all of the following are true:

- `Scheduler`, `rpc::Client`, `rpc::Server`, `Timer`, `EventThread`, `Application`, and the newly Object-derived
  `ManagementService` use `Object`-owned private storage;
- none of those classes declares a second private-data pointer or private per-instance implementation fields in its
  public header;
- `ManagementMutationHandler` no longer exists and successful management mutations emit
  `ManagementService::mutation_committed`;
- `jobud` uses receiver-aware signal connections for management rescans and local connection admission;
- callbacks that represent one accepted operation's completion, a return-producing strategy, or a private non-Object
  adapter remain callbacks;
- all changed public declarations have useful Doxygen comments and all changed public headers remain standalone;
- Linux focused and full regression suites pass, including the real daemon HTTP integration;
- the optional macOS verification is recorded accurately if performed; and
- a separate `JB_BUILD_SQLITE_DRIVER=OFF` configuration builds successfully, without requiring a duplicate full
  no-SQLite CTest run.

## 3. Scope

### 3.1 Included

- private-data migration for every current `Object` subclass that does not follow the established pattern;
- protected private-data constructors for non-final core Object classes that may be subclassed internally;
- out-of-line accessors required after implementation state leaves public headers;
- converting `ManagementService` into an owner-thread `Object`;
- one post-commit management signal and exact emission semantics;
- removal of the management RPC callback seam;
- receiver lifetime tracking for daemon signal connections that capture another `Object`;
- focused behavior, lifetime, public-header, integration, Doxygen, and diagnostic verification;
- repository guidance that prevents the same ownership and notification regressions in Phase 6.

### 3.2 Explicitly excluded

- `jb::core::Process`, process pipes, `SIGCHLD`, process groups, CLI execution, or environment construction;
- changes to `AttemptExecutor`, `HttpClient`, `HttpAttemptExecutor`, libcurl, or HTTP policy;
- replacing `HttpCompletionHandler`, `AttemptCompletionHandler`, or `rpc::MethodHandler` with signals;
- public `job.run_now`, run/attempt queries, cancellation RPC, or any other new wire method;
- scheduler startup recovery, interrupted-attempt handling, or coordinated fatal shutdown;
- schema, migration, repository, transaction, or persistence-format changes;
- pimpl changes to classes that do not derive from `Object`;
- a second full test run with `JB_BUILD_SQLITE_DRIVER=OFF`.

## 4. Non-negotiable design rules

### 4.1 One private block per Object

An `Object` subclass with private state has exactly one heap-allocated private block. That block is passed to the
protected `Object` constructor, stored in `Object::_d`, and deleted by `Object::~Object()` through the virtual
`ObjectPrivate` destructor.

The following are forbidden in an `Object` subclass after this closure:

- `std::unique_ptr<Private> _data` or an equivalent second pimpl pointer;
- direct private instance fields such as a timer handle, event loop, thread, atomic state, or options;
- deleting the result of `d_ptr()` in a derived destructor; and
- passing `*this` into a private constructor while the `Object` base is not yet constructed.

Public `Signal<...>` members remain direct members by design. `Application::s_instance` also remains a static class
member because it is process-global state, not per-instance private data.

Every concrete private structure derives directly or transitively from `jb::core::priv::ObjectPrivate`. An
`IODevice` subclass continues to derive its private structure from `IODevicePrivate`.

Already conformant classes—`IODevice`, `File`, `TcpSocket`, `LocalSocket`, `LocalServer`, `HttpClient`, and
`SystemHttpClient`—are reference implementations and stay behaviorally unchanged. Their source should be edited only
if a closure-stage compile failure exposes a direct dependency on one of the migrated classes.

### 4.2 Safe two-phase owner binding

Base-constructor arguments are evaluated before the derived `Object` is fully constructed. A private structure must
therefore not receive a usable `Derived&` in `new Private{...}`.

When private code needs the public owner, use this pattern:

```cpp
struct Example::Private : jb::core::priv::ObjectPrivate {
    explicit Private(Dependencies... dependencies);

    void bind_owner(Example& value)
    {
        owner = &value;
        // Install receiver-aware signal connections only after Object construction.
    }

    Example* owner{nullptr};
};

Example::Example(Dependencies... dependencies, jb::core::Object* parent)
    : Object(*new Private{dependencies...}, parent)
{
    d_ptr<Private>()->bind_owner(*this);
}
```

`bind_owner()` runs before the public constructor returns. No event, timer, or callback may observe the private block
before binding completes. If binding throws, normal construction unwinding lets `Object` delete the private block.

### 4.3 Access and constness

- non-const methods use `d_ptr<Private>()` or the module's concrete private type;
- const methods use `d_ptr<Private const>()`;
- a method should normally bind one local `data` pointer rather than repeat downcasts;
- private headers remain implementation-only and must not be re-exported by public headers; and
- inline public accessors that need private fields move to the `.cpp` file without changing their signatures.

### 4.4 Destruction ordering

Derived signal members are destroyed before the `Object` base deletes its private block. Each affected destructor must
therefore stop external activity and disconnect or invalidate callbacks while the derived object and its signals still
exist:

- `Scheduler::~Scheduler()` calls `stop()` through its private block;
- `rpc::Client::~Client()` calls `close()`;
- `rpc::Server::~Server()` calls `close()`;
- `Timer::~Timer()` calls `stop()`;
- `EventThread::~EventThread()` calls `quit()` and `wait()`; and
- `Application::~Application()` clears thread context and singleton state before the private event thread is deleted.

Receiver-aware signal connections are preferred whenever a slot captures an `Object`. Context-free connections are
reserved for callables that borrow no `Object`, have process lifetime, or are explicitly disconnected before any
captured target can be destroyed.

### 4.5 Required implementation comments

Add concise code-level rationale comments at the non-obvious ownership and ordering points introduced by this work:

- why an owner back-reference is completed only in the public constructor body;
- why a receiver-aware connection is used when the slot captures another Object;
- why a destructor stops or disconnects activity before derived signal members are destroyed; and
- why management notification occurs after commit but before response encoding.

Do not comment routine `d_ptr()` access or restate assignments. Comments should explain lifetime, ordering, failure,
or reentrancy constraints that are not evident from the statements alone.

## 5. Private-data migration design

### 5.1 `jb::jobu::Scheduler`

Keep `Scheduler::Private` nested and defined in `scheduler.cpp`, but make it inherit
`jb::core::priv::ObjectPrivate`. Include `object_priv.hpp` privately in that translation unit.

Change the private owner reference to an initially null `Scheduler*`. The private constructor continues to own all
dependencies, options, state, the wake timer, and `SchedulerCore`, but it must not receive `Scheduler&`. Add
`bind_owner(Scheduler&)` to:

1. store the owner pointer;
2. connect `wake_timer.timeout` using the scheduler as the receiver; and
3. finish any initialization that needs the public object's EventLoop affinity.

Construct the scheduler with:

```cpp
Scheduler::Scheduler(/* dependencies */, SchedulerOptions options, jb::core::Object* parent)
    : Object(*new Private{/* dependencies */, options}, parent)
{
    d_ptr<Private>()->bind_owner(*this);
}
```

Replace every `_data` access with `d_ptr<Private>()`. Remove `<memory>` from `scheduler.hpp` if no remaining public
declaration needs it. Public signatures, scheduler states, error codes, callback timing, and the `failed` signal remain
unchanged.

The wake timer remains an owned value inside the private block; it must not be assigned the scheduler as an Object
parent because parent ownership is intended for separately heap-allocated children.

### 5.2 `jb::rpc::Client`

Change `Client::Private` in `client_priv.hpp` to derive from `ObjectPrivate` and include `object_priv.hpp` privately.
Its constructor accepts only the borrowed `IODevice` and copied options. Add `bind_owner(Client&)` to:

- store `Client* owner`;
- assert the fully constructed client and device share one non-null EventLoop; and
- install the four existing device connections with the client as receiver.

Keep those connections receiver-aware. Private delivery code emits through `owner->emit(owner->...)` only after owner
binding. The client constructor supplies the private block to `Object`, binds the owner in its body, and retains the
existing `close()` destructor behavior.

Remove `_data` and `<memory>` from `client.hpp`. All request correlation, reentrant write protection, pending limits,
error ordering, close behavior, and public signals remain unchanged.

### 5.3 `jb::rpc::Server`

Change `Server::Private` in `server_priv.hpp` to derive from `ObjectPrivate`. Its constructor accepts only copied
`ServerOptions`; a post-construction bind stores `Server* owner`.

All per-device connections created by `add_connection()` continue to use the server as their receiver. Replace the
private owner reference and every `_data` use with the bound pointer and `d_ptr<Private>()`. Preserve connection-state
maps, method handlers, reentrancy rules, error/close signal ordering, `delete_later()` behavior, and identifier
allocation exactly.

Remove `_data` and `<memory>` from `server.hpp`. The class remains final and needs no protected private-data
constructor.

### 5.4 `jb::core::Timer`

Add `src/core/timer_priv.hpp`:

```cpp
namespace jb::core::priv {

struct TimerPrivate : ObjectPrivate {
    TimerHandle handle;
    Duration    interval{Duration::zero()};
    bool        repeating{false};
};

} // namespace jb::core::priv
```

Forward-declare `priv::TimerPrivate` in `timer.hpp`. The public constructor delegates to a protected private-data
constructor so an internal future subclass can extend the same block:

```cpp
protected:
    explicit Timer(priv::TimerPrivate& dd, Object* parent = nullptr);
```

The actual declaration receives full Doxygen ownership documentation matching `Object` and `IODevice`. Move
`handle()`, `is_active()`, `is_repeating()`, and `set_repeating()` out of line because the public header no longer owns
their fields. Preserve timer restart, one-shot handle clearing, repeat behavior, cancellation, EventLoop affinity, and
the public `timeout` signal.

### 5.5 `jb::core::EventThread`

Add `src/core/event_thread_priv.hpp` with `EventThreadPrivate : ObjectPrivate` containing:

- `std::unique_ptr<EventLoop> event_loop`;
- `std::unique_ptr<std::thread> thread`;
- `std::atomic_bool started` and `finished`; and
- `std::atomic<int> exit_code`.

Add a protected `EventThread(priv::EventThreadPrivate&, Object*)` constructor with complete Doxygen. The public
constructor delegates through a newly allocated private block. Allocate the `EventLoop` in the protected constructor
body, after `Object` has taken ownership of the private block, and then preserve the existing `move_to_thread(this)`
behavior.

Move `as_event_loop()` and `exit_code()` out of line. The worker-thread entry lambda may continue to capture `this`:
the private thread owns it, destruction always calls `quit()` and `wait()`, and the lambda must finish before the
private block is deleted. Every field access inside and outside the lambda goes through `d_ptr<EventThreadPrivate>()`.

Preserve memory ordering, start/exec distinction, failure exit code, signal order, and thread-context installation and
cleanup.

### 5.6 `jb::core::Application`

Add `src/core/application_priv.hpp` with `ApplicationPrivate : ObjectPrivate` containing the argument pointers,
exit code, and `std::unique_ptr<EventThread>`.

Add a protected constructor for internal private-data extension:

```cpp
protected:
    Application(priv::ApplicationPrivate& dd, int argc, char const* argv[]);
```

The public constructor delegates through a new `ApplicationPrivate`. The protected constructor initializes the
private arguments and event thread, enforces the singleton, installs the EventLoop in `ThreadCtx`, and preserves
`move_to_thread()` behavior. Move `thread()` and `exit_code()` out of line. Keep `s_instance` static in the class.

Destruction must clear the thread context and singleton before `Object` deletes `ApplicationPrivate` and its owned
`EventThread`. Preserve singleton mismatch handling, event delivery, exit code behavior, and signal order.

### 5.7 `jb::jobu::ManagementService`

Change the final class to derive from `jb::core::Object`. Its nested `Private` derives from `ObjectPrivate` and keeps
the existing borrowed collaborators, defaults, repositories, and initialization error.

Extend the constructor with an optional parent after the existing default layer:

```cpp
ManagementService(jb::db::Database& database,
                  AttributeRegistry const& attributes,
                  CronEngine const& cron,
                  jb::core::UuidGenerator& uuid_generator,
                  jb::core::TimeSource& time_source,
                  AttributeSet daemon_defaults = {},
                  jb::core::Object* parent = nullptr);
```

Existing call sites remain source-compatible. The constructor passes `*new Private{...}` and `parent` to `Object`.
The destructor becomes `override`. Remove `_data` and replace all access with `d_ptr<Private>()`.

In Stage 5.26, update the class Doxygen to state that construction and all operations use the Object/database owner
thread and that parent ownership is optional. Stage 5.27 then adds the post-commit signal contract from section 6.
Public headers include only the project-owned `object.hpp` and, once the signal is added, `signal.hpp`;
`object_priv.hpp` remains private to `management.cpp`.

## 6. Management mutation signal

### 6.1 Public contract

Add this public signal to `ManagementService`:

```cpp
/// Emitted synchronously once after a management mutation has committed successfully.
jb::core::Signal<> mutation_committed;
```

The complete Doxygen must specify:

- emission occurs on the service/database owner thread;
- the transaction has committed before emission;
- state returned by the method is durable before any slot runs;
- reads and failures do not emit;
- successful idempotency replays and successful lifecycle no-ops do emit, matching the existing RPC rescan behavior;
- `run_now()` emits even though public `job.run_now` RPC remains deferred;
- a direct slot may request coalesced later work, but must not block, start nested event processing, re-enter
  `ManagementService`, or use the same database during emission; and
- slots must not destroy the service before signal delivery returns.

The signal is emitted exactly once on every successful return path from:

- `create_queue`, `update_queue`, `suspend_queue`, `resume_queue`, and `delete_queue`;
- `create_job`, `update_job`, `suspend_job`, `resume_job`, `move_job`, and `delete_job`; and
- `run_now`.

It is never emitted by queue/job get or list operations, constructor failure, validation failure, transaction failure,
or any other failed result.

Add a small private `emit_mutation_committed()` member to centralize the protected `emit()` call. Each service method
still controls placement: invoke the helper only after a successful commit/replay/no-op path and immediately before
returning success. Do not put emission in repositories, transaction destructors, or generic result helpers where the
durable boundary becomes unclear.

### 6.2 RPC adapter

Delete `ManagementMutationHandler`, remove `<functional>` when unused, and restore the registration API to:

```cpp
auto register_management_methods(jb::rpc::Server& server,
                                 ManagementService& service,
                                 AttributeRegistry const& attributes) -> bool;
```

Remove the copied callback from all installed method handlers and delete `notify_committed_mutation()`. The adapter
continues to call the service and then encode its result. Because the service emits before returning, a committed
mutation still notifies observers even when response encoding subsequently fails.

The exact 15 registered management method names and API version remain unchanged. Do not register `job.run_now` in
this closure.

### 6.3 Daemon wiring

Replace the callback argument to `register_management_methods()` with a receiver-aware connection established before
the server starts accepting requests:

```cpp
management_service.mutation_committed.connect(&scheduler, [&scheduler]() -> void {
    scheduler.request_rescan();
});
```

`request_rescan()` remains non-reentrant with respect to persistence: it only coalesces a zero-delay scheduler wake.
The scheduler is the receiver, so its destruction deactivates the connection.

Also make local connection admission receiver-aware:

```cpp
local_server.new_connection.connect(&rpc_server, [&local_server, &rpc_server]() -> void {
    // Existing admission loop.
});
```

This prevents the stored slot from retaining a live reference to `rpc_server` after reverse local-variable
destruction has destroyed that receiver. Logging-only context-free slots that capture no Object may remain unchanged.

## 7. Callback versus signal decisions

The closure must not perform a mechanical replacement of every `std::function`.

| Mechanism | Decision | Reason |
| --- | --- | --- |
| `ManagementMutationHandler` | Replace with signal | Reusable no-return event notification with a natural Object sender and potentially multiple observers. |
| `HttpCompletionHandler` | Keep callback | One owning, exactly-once completion obligation belongs to one accepted request. |
| `AttemptCompletionHandler` | Keep callback | One continuation belongs to one durable attempt accepted by an executor. |
| `rpc::MethodHandler` | Keep callback | Named request strategy must synchronously produce one `MethodResult`. |
| `Task` and `FdCallback` | Keep callback | EventLoop operations and backend readiness continuations, not public broadcasts. |
| Private curl handlers | Keep callback | Tight non-Object implementation composition and C callback adaptation. |
| `SchedulerCoreCallbacks` | Keep callback | Private deterministic core is deliberately not an Object; public scheduler already exposes `failed`. |
| Thread entry lambdas | Keep callback | Owned executable work with explicit join semantics, not observable events. |

Any new Phase 6 design must apply the same semantic test: use a signal for an Object's reusable observable event; use
a callback for one transferred completion, a strategy with a return value, or a private non-Object adapter seam.

## 8. Planned file changes

| Area | Files |
| --- | --- |
| Scheduler | `src/jobu/scheduler.hpp`, `src/jobu/scheduler.cpp`, scheduler public-header/EventLoop tests |
| RPC client | `src/rpc/client.hpp`, `src/rpc/client_priv.hpp`, `src/rpc/client.cpp`, RPC tests |
| RPC server | `src/rpc/server.hpp`, `src/rpc/server_priv.hpp`, `src/rpc/server.cpp`, RPC tests |
| Timer | `src/core/timer.hpp`, `src/core/timer.cpp`, new `src/core/timer_priv.hpp`, `test/timer-test.cpp` |
| Event thread | `src/core/event_thread.hpp`, `src/core/event_thread.cpp`, new `src/core/event_thread_priv.hpp`, `test/event-thread-test.cpp` |
| Application | `src/core/application.hpp`, `src/core/application.cpp`, new `src/core/application_priv.hpp`, `test/application-test.cpp` |
| Management service | `src/jobu/management.hpp`, `src/jobu/management.cpp`, management/public-header tests |
| RPC/daemon signal wiring | `src/jobu/management_rpc.hpp`, `src/jobu/management_rpc.cpp`, `src/jobud/main.cpp`, management RPC and daemon tests |
| Policy/docs | `AGENTS.md`, Phase 5 planning text, stale integration-test comment, planning index if this document is committed |

No CMake production target, public library dependency, schema file, or platform backend file should need to change.
Adding a new private header to an existing target's source list is optional when that target already uses directory
sources only for IDE presentation; it must never become a public include dependency.

## 9. Testing strategy

### 9.1 Structural acceptance

Review all current `Object` subclasses after the migrations. The only direct state allowed in their public class
definitions is:

- public signal members;
- static state such as `Application::s_instance`; and
- no-state declarations and methods.

The review must explicitly confirm:

- no affected header contains `_data`, `unique_ptr<Private>`, or migrated implementation fields;
- every affected private structure inherits `ObjectPrivate`;
- every constructor transfers exactly one private allocation to its base;
- owner back-references are bound only after the Object base is constructed;
- no derived destructor deletes private data; and
- const accessors use a const private pointer.

This is a source invariant, not an ABI-size test. Do not add brittle tests that parse headers or assert private object
sizes.

### 9.2 Behavior and lifetime coverage

The existing tests remain the main regression oracle. Extend them only where the migration exposes a missing lifetime
case:

- Scheduler: armed timer destruction, stop, failure emission, rescan coalescing, and completion after stop;
- RPC client: receiver-aware device disconnect, pending request close, reentrant signal behavior, and destruction;
- RPC server: connection admission/retirement, error-before-close ordering, handler reentrancy, and destruction;
- Timer: initial state, one-shot handle clearing, repeating restart, stop, and destruction with an armed timer;
- EventThread: start/exec, signal order, quit/wait, invalid backend, and destruction after start;
- Application: singleton lifecycle, EventLoop installation, quit/exit code, and signal order; and
- ManagementService: exact mutation signal counts, read/failure silence, replay/no-op behavior, run-now behavior,
  transaction failure, receiver destruction, and post-commit/pre-encoding ordering.

Tests must stay deterministic. Do not add arbitrary sleeps; use existing EventLoop processing, explicit server
readiness, conditions, and bounded integration deadlines.

### 9.3 Public-header and Doxygen gates

For every changed public header:

- it compiles through the existing first-include or public-header target;
- it includes what its declarations require and does not include a private header;
- every new protected constructor, parent parameter, ownership rule, signal, timing rule, and reentrancy restriction
  has Doxygen; and
- comments that still say management methods invoke no callbacks are revised to distinguish signal emission from
  external work.

### 9.4 Diagnostics

After each stage, run clangd diagnostics for changed standalone C++ headers and translation units using the matching
build configuration. Enabled compiler, include-cleaner, and clang-tidy diagnostics are blockers. Do not modify
`.clangd`, introduce suppressions, or expand into unrelated cleanup.

## 10. Implementation stages

Each stage is independently reviewable. Codex implements exactly one stage, runs its listed validation, reports
changed files and results, and stops for approval before the next stage.

### Stage 5.20: Move Scheduler state into Object private storage

Implement section 5.1 only.

Required work:

- derive `Scheduler::Private` from `ObjectPrivate`;
- introduce safe post-base owner binding;
- make the wake-timer connection receiver-aware;
- construct the scheduler through `Object(*new Private..., parent)`;
- replace every `_data` access and remove the member; and
- preserve public Doxygen and header independence.

Verification:

- build `jobu-scheduler-public-header-test`, `scheduler-event-loop-test`, and `scheduler-integration-test`;
- run those tests plus scheduler dispatch/core tests where available;
- cover destruction with an armed wake and existing failure-signal behavior;
- run diagnostics on `scheduler.hpp` and `scheduler.cpp`; and
- confirm no HTTP, schema, or public scheduler behavior changed.

Exit: `Scheduler` has one Object-owned private block and unchanged deterministic behavior.

Suggested commit subject: `store scheduler state in Object private data`

Stop for review.

### Stage 5.21: Move RPC Client state into Object private storage

Implement section 5.2 only.

Verification:

- build and run `rpc-client-test` and `rpc-public-headers-test`;
- verify all four device connections still use the client receiver;
- exercise close, destruction, pending failure order, and reentrant signals;
- run changed-file diagnostics; and
- confirm no RPC wire or error-code change.

Exit: `rpc::Client` has one Object-owned private block and unchanged correlation behavior.

Suggested commit subject: `store RPC client state in Object private data`

Stop for review.

### Stage 5.22: Move RPC Server state into Object private storage

Implement section 5.3 only.

Verification:

- build and run `rpc-server-test` and `rpc-public-headers-test`;
- run management/system-info RPC tests needed to exercise registered handlers;
- verify connection-error/closed ordering and receiver-aware device subscriptions;
- run changed-file diagnostics; and
- confirm method registration and wire behavior are unchanged.

Exit: `rpc::Server` has one Object-owned private block and unchanged connection behavior.

Suggested commit subject: `store RPC server state in Object private data`

Stop for review.

### Stage 5.23: Move Timer state into Object private storage

Implement section 5.4 only, including `timer_priv.hpp`, out-of-line accessors, and the protected private-data
constructor.

Verification:

- build and run `timer-test`, `object-test`, and `signal-test`;
- verify one-shot, repeating, restart, stop, and armed destruction cases;
- compile `timer.hpp` as the first include through its existing test path;
- run diagnostics on the public header, implementation, private header through `timer.cpp`, and tests.

Exit: Timer state is absent from the public class and still obeys its complete EventLoop contract.

Suggested commit subject: `store timer state in Object private data`

Stop for review.

### Stage 5.24: Move EventThread state into Object private storage

Implement section 5.5 only.

Verification:

- build and run `event-thread-test`, `event-loop-test`, `signal-test`, and `object-test`;
- verify thread-context installation/cleanup, start/exec distinction, quit/wait, and signal order;
- exercise destruction of both never-started and completed threads;
- run changed-file diagnostics, using `event_thread.cpp` as the owning context for its private header.

Exit: EventThread has one Object-owned private block with unchanged synchronization behavior.

Suggested commit subject: `store event thread state in Object private data`

Stop for review.

### Stage 5.25: Move Application state into Object private storage

Implement section 5.6 only.

Verification:

- build and run `application-test`, `event-thread-test`, `event-loop-test`, `object-test`, and `signal-test`;
- verify singleton construction/destruction, EventLoop affinity, exit codes, and start/quit signal ordering;
- confirm `s_instance` is the only retained non-signal class state and is static;
- run changed-file diagnostics.

Exit: Application has one Object-owned per-instance private block and unchanged process-global singleton semantics.

Suggested commit subject: `store application state in Object private data`

Stop for review.

### Stage 5.26: Make ManagementService an Object

Implement only the inheritance, constructor, parent ownership, private-block migration, destructor override, and
Doxygen changes from section 5.7. Do not add or wire the signal yet; the existing RPC callback keeps daemon behavior
unchanged at this intermediate boundary.

Verification:

- build `jobu-management-public-header-test` and all management service tests;
- run queue, job, cron/run-now, and management JSON tests;
- verify old constructor call sites compile unchanged;
- add focused parent-ownership/destruction coverage without allowing parent deletion of stack objects;
- run changed-file diagnostics.

Exit: ManagementService is an owner-thread Object with one Object-owned private block and no behavior change.

Suggested commit subject: `make management service an Object`

Stop for review.

### Stage 5.27: Add the post-commit management signal

Implement section 6.1 while temporarily retaining the RPC adapter callback.

Verification must cover every mutating method family:

- one signal for fresh queue/job/run-now success;
- one signal for successful idempotency replay;
- one signal for successful suspend/resume no-op behavior;
- no signal for get/list operations;
- no signal for validation, not-found, conflict, injected commit, or repository failure;
- receiver destruction disconnects a receiver-aware slot; and
- a slot that only records/request-rescans does not cause nested database work.

Build and run management service, cron/run-now, scheduler-management interaction, signal, and public-header tests.
Run changed-file diagnostics and audit all signal Doxygen.

Exit: the service owns the complete mutation notification contract; existing daemon callback wiring still operates.

Suggested commit subject: `signal committed management mutations`

Stop for review.

### Stage 5.28: Remove mutation callbacks and use receiver-aware daemon signals

Implement sections 6.2 and 6.3.

Required work:

- delete `ManagementMutationHandler` and all adapter callback copies;
- update public-header type assertions and registration call sites;
- connect the service signal to the scheduler receiver in `jobud`;
- make local connection admission use `rpc_server` as receiver;
- preserve notification before failed response encoding;
- update the stale Stage 5.15 test comment to describe the actual direct-service/signal boundary; and
- keep `job.run_now` unregistered.

Verification:

- build and run `jobu-management-rpc-public-header-test`, `management-rpc-test`, `rpc-server-test`,
  `scheduler-event-loop-test`, and `jobud-http-integration-test`;
- prove successful mutating RPCs notify once and reads/failures do not;
- prove a committed mutation followed by encoding failure has already emitted;
- prove an RPC-created due HTTP job wakes the live daemon scheduler;
- inspect all daemon signal connections for captured Object lifetime; and
- run changed-file diagnostics.

Exit: public mutation callback machinery is gone and daemon Object notifications use lifetime-aware signals.

Suggested commit subject: `wire management scheduling through signals`

Stop for review.

### Stage 5.29: Codify Object and signal policy

Update `AGENTS.md` and planning documentation so Phase 6 Codex work follows the settled rules:

- Object subclasses extend the inherited private block rather than add another pimpl or direct state;
- owner binding occurs after base construction;
- Object-capturing signal slots use receiver-aware connections;
- signals represent reusable Object events;
- one-shot completions, return-producing strategies, and private non-Object seams remain callbacks; and
- public Doxygen plus rationale comments are stage completion gates.

Correct the Phase 5 section 15.3 ambiguity: the Phase 5 RPC method set did not add `job.run_now`; the service-level
`run_now()` signal behavior is covered by this closure without adding a wire method. Add this closure document to the
planning index when it is committed to the repository.

Verification:

- review policy wording against `object.hpp`, `signal.hpp`, and the callback table in section 7;
- confirm no historical document now claims a new RPC capability;
- run Markdown link/code-fence checks used by the repository; and
- make no source behavior change.

Exit: the rules are explicit inputs to Phase 6 rather than conventions inferred from examples.

Suggested commit subject: `document Object private data and signal policy`

Stop for review.

### Stage 5.30: Linux closure integration and regression

On Linux, build the complete normal configuration and run the focused cross-module matrix:

- Object, signal, timer, application, EventThread, and EventLoop tests;
- RPC client/server and public-header tests;
- management service, management RPC, run-now, scheduler EventLoop, and scheduler integration tests; and
- real HTTP scheduler and `jobud` HTTP integration tests.

Perform the structural audit from section 9.1 and a Doxygen/public-header audit. Run diagnostics for all C++ files
changed in Stages 5.20-5.28. Fix only closure-related findings; stop and request a revised plan for unrelated defects.

Exit: all migrated lifetimes and the real Phase 5 daemon path work together on Linux.

Suggested commit subject only if fixes are required: `complete Phase 5 closure regression`

Stop for review before optional macOS work.

### Stage 5.31 (optional): macOS closure verification

Build the normal configuration on macOS and run the full registered suite, with focused attention to:

- Object destruction and receiver disconnection;
- Timer and EventThread behavior on the kqueue EventLoop;
- RPC and local-socket connection admission;
- management mutation wake-up; and
- real HTTP scheduler/daemon integration.

No platform-specific production change is expected. If a real Darwin-only defect is found, make the smallest isolated
change, run diagnostics using the matching macOS compilation database, rerun affected tests, and record exact macOS,
Apple Clang, CMake, Ninja, curl, OpenSSL, and SQLite versions that are actually available.

Exit: the closure is verified on macOS, or the handoff explicitly records that this optional stage was not run.

Suggested commit subject only if changes are required: `verify Phase 5 closure on macOS`

Stop for review.

### Stage 5.32: Final clean Linux verification and handoff

From clean build directories, run:

```bash
cmake -S . -B .bld-phase5-close -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld-phase5-close
ctest --test-dir .bld-phase5-close/test --output-on-failure

cmake -S . -B .bld-phase5-close-no-sqlite -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DJB_BUILD_SQLITE_DRIVER=OFF
cmake --build .bld-phase5-close-no-sqlite
```

Do not run a second full CTest suite in the SQLite-disabled tree. Its purpose is to prove generic `core`, `rpc`, `net`,
`net-http`, `jobu`, and `jobu-http` build boundaries remain independent of SQLite.

Final audits:

- re-run section 9.1's complete Object subclass inventory;
- confirm `ManagementMutationHandler` and its callback parameter are absent;
- confirm the retained callback types match section 7;
- confirm public-header and Doxygen coverage;
- confirm no schema version/DDL, HTTP behavior, process runner, or RPC method-set change;
- confirm the source worktree is clean after committed work; and
- write a closure handoff with exact revision, Linux toolchain/dependency versions, build steps, test count, optional
  macOS evidence, and the Phase 6 entry boundary.

Exit: Phase 5 is closed and Phase 6 may be planned against the new ownership and signal baseline.

Suggested commit subject: `document Phase 5 closure verification`

## 11. Final acceptance checklist

- [ ] `Scheduler` uses `Object`-owned private data.
- [ ] `rpc::Client` uses `Object`-owned private data.
- [ ] `rpc::Server` uses `Object`-owned private data.
- [ ] `Timer` uses `Object`-owned private data and supports internal private-data extension.
- [ ] `EventThread` uses `Object`-owned private data and supports internal private-data extension.
- [ ] `Application` uses `Object`-owned per-instance private data; only its singleton pointer remains static.
- [ ] `ManagementService` derives from `Object`, accepts an optional parent, and uses the inherited private block.
- [ ] No affected Object subclass declares a second pimpl pointer or direct private implementation fields.
- [ ] Owner pointers and signal receivers are bound only after Object base construction.
- [ ] Destructors quiesce callbacks before derived signals and private data are destroyed.
- [ ] `ManagementService::mutation_committed` is fully documented and emitted exactly once for all successful
      mutation paths, including replay/no-op/run-now.
- [ ] Reads and all failed mutations emit no management signal.
- [ ] `ManagementMutationHandler` is removed from the public API and implementation.
- [ ] Management scheduler wake-up and local RPC admission use receiver-aware signal connections.
- [ ] HTTP, attempt, RPC-method, EventLoop, curl-adapter, scheduler-core, and thread-entry callbacks remain callbacks.
- [ ] The 15-method management RPC set and API version are unchanged; `job.run_now` remains deferred.
- [ ] No schema, HTTP policy, Phase 6 runner, or Phase 7 recovery/shutdown work was added.
- [ ] Every changed public declaration has useful Doxygen and every public header remains standalone.
- [ ] Changed C++ files are compiler/include-cleaner/clang-tidy diagnostics-clean.
- [ ] Focused Linux integration passes.
- [ ] Optional macOS status is recorded without invented evidence.
- [ ] Fresh SQLite-enabled Linux build and complete CTest pass.
- [ ] Fresh SQLite-disabled build passes; no duplicate no-SQLite full CTest is required.

## 12. Phase 6 entry boundary

Phase 6 may assume:

- every existing Object-derived production component demonstrates the correct single-private-block pattern;
- new process-related Objects must follow the same construction and destruction rules;
- management changes wake the scheduler through a lifetime-aware signal;
- exact request/attempt completions remain explicit callbacks;
- the real HTTP runner and runner-neutral output persistence remain stable; and
- CLI candidates remain pending because no CLI-capable executor exists yet.

Phase 6 must not reopen these closure decisions unless implementation evidence shows a concrete conflict. Any such
conflict requires a revised design and approval before code changes continue.
