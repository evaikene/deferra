# JobU Phase 5 — Stage 5.4a Code-Level Design

Status: implementation-ready design  
Baseline: `main` at `5c80f21c18e41465aba7ca8f38bfcc7e85113429`, after Stage 5.4  
Prepared: 2026-08-28  
Parent design: `docs/planning/jobu-phase5-code-design.md`

## 1. Purpose

Stage 5.4a corrects a `jb::core::EventLoop` readiness-contract deficit exposed by the Stage 5.4 libcurl integration.

The existing EventLoop implementation uses edge-triggered file-descriptor readiness on both supported native backends:

- Linux epoll registrations use `EPOLLET`;
- kqueue read/write filters use `EV_CLEAR`.

That behavior is not represented in the public `EventLoop::watch_fd()` contract. Existing project-owned socket implementations work because they own their native I/O and drain read/write/accept operations until `EAGAIN` or `EWOULDBLOCK`.

The Stage 5 HTTP adapter is different. libcurl owns the socket operations. One `curl_multi_socket_action()` call may perform bounded work while retaining the same read/write interest. The adapter cannot safely continue calling libcurl until `EAGAIN`, because it does not perform or observe the native socket operation itself.

Stage 5.4 currently compensates on Linux by replacing an unchanged EventLoop watch after every readiness drive. That causes epoll to execute `EPOLL_CTL_MOD`, which makes sustained uploads progress. The same workaround is ineffective on kqueue because unchanged registrations deliberately cause no native filter operation.

Stage 5.4a therefore makes readiness trigger semantics explicit in `EventLoop`, retains edge-triggered behavior for existing socket owners, gives libcurl true level-triggered watches, and removes the Stage 5.4 refresh workaround.

This stage is a prerequisite for Stage 5.5. Do not continue Phase 5 implementation until Stage 5.4a is complete and reviewed.

---

## 2. Scope

### 2.1 In scope

- Add an explicit public file-descriptor trigger mode to `jb::core`.
- Require every `EventLoop::watch_fd()` caller to select `Edge` or `Level`.
- Document the behavioral contract of both modes.
- Extend EventLoop's private watch state to retain the selected mode.
- Extend the EventLoop backend contract to receive the selected mode.
- Map edge/level modes correctly to epoll.
- Map edge/level modes correctly to kqueue.
- Preserve transactional watch replacement and rollback behavior.
- Make callback-only replacement with unchanged native registration independent of backend-specific rearm behavior.
- Extend deterministic fake-backend coverage for trigger modes.
- Extend kqueue transition tests for trigger-mode changes and rollback.
- Migrate all existing core-owned socket/server watches explicitly to edge-triggered mode.
- Migrate `CurlMultiAdapter` to level-triggered mode.
- Remove the Stage 5.4 unchanged-watch refresh workaround completely.
- Add native behavioral EventLoop tests demonstrating edge and level semantics.
- Re-run Stage 5.4 sustained upload/download integration coverage on the corrected abstraction.
- Complete Doxygen and public-header coverage for the changed public EventLoop API.

### 2.2 Explicitly out of scope

- A public `rearm_fd()` API.
- Making all EventLoop watches level-triggered.
- Changing `FdWatch` identity or stale-handle semantics.
- Multiple independent watches for one file descriptor.
- Separate trigger modes for read and write on the same descriptor.
- One-shot readiness.
- Changing EventLoop timer semantics.
- Changing EventLoop task/event dispatch order.
- Changing native error reporting or the EventLoop exception policy.
- Refactoring socket buffering or lifecycle behavior.
- Changing libcurl drive, completion, request, response, timeout, redirect, TLS, or proxy policy except where required to remove the watch-refresh workaround.
- Stage 5.5 timeout/cancellation/error-mapping work.
- Any JobU scheduler, persistence, RPC, schema, or attribute change.
- Adding another EventLoop backend.

---

## 3. Non-negotiable decisions

| Decision | Required result |
|---|---|
| Trigger mode | Every public fd watch explicitly selects `Edge` or `Level`. |
| No default | `watch_fd()` has no default trigger mode. Existing and future callers must make their readiness assumption visible. |
| Edge contract | The consumer owns readiness draining and must not depend on another callback until the indicated operation has been consumed to would-block. |
| Level contract | Readiness may be reported repeatedly while the requested condition remains true. |
| Existing networking | `TcpSocket`, `LocalSocket`, and `LocalServer` continue using edge-triggered watches. |
| libcurl | Every curl-managed socket uses level-triggered readiness. |
| epoll mapping | Edge includes `EPOLLET`; Level omits it. |
| kqueue mapping | Edge includes `EV_CLEAR`; Level omits it. |
| Replacement | Callback, event mask, and trigger mode are replaced only after any required native registration change succeeds. |
| Callback-only replacement | If event mask and trigger mode are unchanged, EventLoop performs no native backend operation and only replaces the callback. |
| No rearm implication | Re-registering an unchanged public watch is not an EventLoop rearm contract. |
| Rollback | Failed native replacement retains the old public registration. A failed backend rollback makes the backend unusable, consistent with the current contract. |
| Curl workaround | `refresh_socket_watch_after_readiness()` and all associated state/tests are removed. |

---

## 4. Public API change

### 4.1 `event_loop_types.hpp`

Add:

```cpp
/// Readiness notification mode for an EventLoop file-descriptor watch.
enum class FdTriggerMode : std::uint8_t {
    Edge,
    Level,
};
```

The actual Doxygen must describe both values sufficiently for a caller to choose correctly.

Required semantics:

### `FdTriggerMode::Edge`

An edge watch reports readiness transitions rather than continuously reporting the current ready state.

A consumer using an edge watch is responsible for consuming the indicated operation until it would block before relying on a later readiness callback.

For normal socket/file-descriptor I/O this means:

- the descriptor is nonblocking;
- read operations continue until `EAGAIN`/`EWOULDBLOCK`;
- write operations continue until the output is drained or `EAGAIN`/`EWOULDBLOCK`;
- listener accept loops continue until `EAGAIN`/`EWOULDBLOCK`.

Failure to satisfy this contract may leave the descriptor ready without another callback.

### `FdTriggerMode::Level`

A level watch reports the current readiness state.

The callback may therefore be invoked again on a later EventLoop poll even if no new readiness transition occurred.

The consumer:

- does not need to own or completely drain the native operation;
- must nevertheless make progress, change/remove its watch, or otherwise tolerate repeated callbacks;
- must not assume that one readiness callback corresponds to one readiness transition.

EventLoop performs no suppression of repeated level callbacks.

### 4.2 `event_loop.hpp`

Change:

```cpp
auto watch_fd(int fd, FdEvents events, FdCallback callback) -> FdWatch;
```

to:

```cpp
auto watch_fd(int fd,
              FdEvents events,
              FdTriggerMode trigger_mode,
              FdCallback callback) -> FdWatch;
```

Do not provide:

- an overload with the old signature;
- a default value for `trigger_mode`;
- convenience methods such as `watch_fd_edge()` or `watch_fd_level()`.

The compile break is intentional and is used to find every caller whose readiness assumption must now be explicit.

Update the class-level `EventLoop` example accordingly.

### 4.3 `watch_fd()` Doxygen

The public method documentation must state:

- `fd`, event-mask, callback, and owner-thread validation remain unchanged;
- the selected trigger mode is part of the native registration;
- edge and level semantics as defined above;
- replacing a watch replaces callback, events, and trigger mode;
- if events or trigger mode require a backend change and that change fails, the previous watch remains active;
- replacing only the callback when events and trigger mode are unchanged does not perform a native registration operation;
- callback-only replacement must not be used as a rearm mechanism;
- the method remains owner-thread-only.

---

## 5. EventLoop internal implementation

### 5.1 Watch state

Change:

```cpp
struct WatchEntry {
    FdCallback callback;
    FdEvents   events;
};
```

to:

```cpp
struct WatchEntry {
    FdCallback    callback;
    FdEvents      events;
    FdTriggerMode trigger_mode;
};
```

The trigger mode is retained as part of EventLoop's representation of the active native registration.

### 5.2 `watch_fd()` replacement algorithm

Use this logical ordering:

```text
1. Validate owner thread, backend, fd, events, and callback.
2. Look for an existing public watch for fd.
3. If an existing watch has the same event mask and trigger mode:
     - replace only its callback;
     - perform no backend operation;
     - return the existing fd watch handle.
4. Otherwise call backend->add_fd(fd, events, trigger_mode).
5. If backend registration fails:
     - leave the existing WatchEntry completely unchanged;
     - return an invalid FdWatch.
6. After backend registration succeeds:
     - replace/store callback, events, and trigger mode together;
     - return FdWatch{fd}.
```

This explicit callback-only fast path is important.

Today epoll performs `EPOLL_CTL_MOD` for unchanged watches while kqueue performs no operation. Leaving that difference in place would continue to give an unchanged public operation different side effects on the two platforms.

After Stage 5.4a:

```cpp
watch_fd(fd, same_events, same_mode, new_callback);
```

means only "replace this callback."

It never means "rearm", "refresh", or "re-evaluate readiness."

### 5.3 `unwatch_fd()`

No public signature change is required.

Removal remains:

1. validate thread;
2. locate active public watch;
3. ask backend to remove the fd;
4. retain the WatchEntry if backend removal fails;
5. erase it only after backend removal succeeds.

Trigger mode does not change the removal contract.

---

## 6. Private EventLoop backend contract

### 6.1 `event_loop_backend.hpp`

Change:

```cpp
virtual auto add_fd(int fd, FdEvents events) -> bool = 0;
```

to:

```cpp
virtual auto add_fd(int fd,
                    FdEvents events,
                    FdTriggerMode trigger_mode) -> bool = 0;
```

Update the private documentation to state that:

- the backend applies or replaces the complete native registration represented by `{events, trigger_mode}`;
- a successful call means the requested registration is native state;
- failure means either the old registration was retained/restored or the backend became unusable;
- registration changes remain transactional.

Do not add a backend `rearm_fd()` operation.

---

## 7. epoll backend

### 7.1 Registration mapping

Current epoll registrations unconditionally include `EPOLLET`.

Change native event construction to:

```cpp
auto native_events = to_epoll(events);
if (trigger_mode == FdTriggerMode::Edge) {
    native_events |= EPOLLET;
}
```

The resulting mapping is:

| EventLoop | epoll |
|---|---|
| `Edge` | requested readiness bits + `EPOLLET` |
| `Level` | requested readiness bits without `EPOLLET` |

Wakeup-fd registration is internal EventLoop machinery and remains unchanged.

### 7.2 Add/modify behavior

Retain the current:

```text
EPOLL_CTL_MOD
    -> if ENOENT: EPOLL_CTL_ADD
```

behavior.

A trigger-mode change on an existing descriptor naturally reaches `EPOLL_CTL_MOD` because EventLoop calls the backend whenever either:

- the event mask changes; or
- the trigger mode changes.

A callback-only replacement does not reach epoll at all.

### 7.3 Failure semantics

Do not add delete/re-add logic for trigger changes.

`EPOLL_CTL_MOD` is the native replacement operation. If it fails for an existing registration, the old registration remains the EventLoop representation.

Existing backend logging/error behavior remains unchanged.

---

## 8. kqueue backend

The kqueue implementation requires the most important structural change in this stage.

### 8.1 Registration state

Current bookkeeping stores only:

```cpp
std::unordered_map<int, FdEvents> _registered;
```

Replace this with a complete registration type in the private kqueue boundary:

```cpp
struct KqueueFdRegistration {
    FdEvents      events;
    FdTriggerMode trigger_mode{FdTriggerMode::Edge};
};
```

and store:

```cpp
std::unordered_map<int, KqueueFdRegistration> _registered;
```

Only descriptors with at least one enabled read/write filter belong in `_registered`.

### 8.2 Per-filter native mode

Introduce a private representation suitable for transactional filter changes:

```cpp
enum class KqueueFilterMode : std::uint8_t {
    Disabled,
    Edge,
    Level,
};
```

For a given `FdEvent`:

```text
event absent from registration -> Disabled
event present + Edge trigger    -> Edge
event present + Level trigger   -> Level
```

This avoids representing an irrelevant trigger mode for a disabled filter.

### 8.3 Native mapping

Map filter mode as:

| KqueueFilterMode | kevent operation |
|---|---|
| `Disabled` | `EV_DELETE` |
| `Edge` | `EV_ADD | EV_CLEAR` |
| `Level` | `EV_ADD` |

`EV_ADD` on an existing kqueue filter is deliberately used for trigger-mode changes. Re-adding an existing event modifies its parameters without creating a duplicate filter.

Do not implement Edge↔Level transitions using a normal delete followed by a later add.

### 8.4 Transaction helper redesign

Update `event_loop_backend_kqueue_priv.hpp` so the transition helper compares complete per-filter modes rather than only enabled/disabled state.

Conceptually:

```cpp
template <typename ApplyFilter>
auto transition_kqueue_filters(KqueueFdRegistration current,
                               KqueueFdRegistration requested,
                               ApplyFilter&& apply_filter)
    -> KqueueTransitionStatus;
```

For each of `Read` and `Write`:

```text
1. Derive current KqueueFilterMode.
2. Derive requested KqueueFilterMode.
3. If they are identical:
     - do nothing.
4. Otherwise:
     - apply the requested filter mode.
5. Record each successfully changed filter.
```

If an operation fails:

```text
1. Assume the failed operation itself did not alter native state, matching
   the existing apply callback contract.
2. Roll back successfully changed filters in reverse order.
3. Restore each changed filter to its original KqueueFilterMode.
4. If all rollback operations succeed:
     -> FailedRolledBack.
5. If any rollback operation fails:
     -> RollbackFailed.
```

This handles all combinations uniformly:

```text
Disabled -> Edge
Disabled -> Level
Edge     -> Disabled
Level    -> Disabled
Edge     -> Level
Level    -> Edge
```

### 8.5 Transition result

The transition helper no longer needs to manufacture a complete "current registration" after every operation.

Prefer reducing:

```cpp
struct KqueueTransitionResult {
    KqueueTransitionStatus status;
    FdEvents               events;
};
```

to returning `KqueueTransitionStatus` directly.

Reason:

- `Applied`: caller knows native state equals `requested`;
- `FailedRolledBack`: caller knows native state equals `current`;
- `RollbackFailed`: native state may no longer be representable by one `{events, trigger_mode}` registration and the backend is immediately unusable anyway.

This avoids pretending that a partially failed rollback can always be represented accurately.

### 8.6 `KqueueBackend::add_fd()`

Required flow:

```text
1. Reject when backend is unhealthy.
2. Find existing KqueueFdRegistration.
3. If absent, synthesize an empty current registration whose trigger mode
   equals the requested mode; the trigger value is irrelevant while no
   filters are enabled.
4. requested = {events, trigger_mode}.
5. Run transition_kqueue_filters(current, requested, ...).
6. Applied:
     - store requested in _registered;
     - return true.
7. FailedRolledBack:
     - leave _registered unchanged;
     - return false.
8. RollbackFailed:
     - leave bookkeeping unchanged;
     - mark backend unhealthy;
     - log the rollback failure;
     - return false.
```

### 8.7 `KqueueBackend::remove_fd()`

Required flow:

```text
1. Reject when backend is unhealthy.
2. If fd is absent from _registered:
     - return true.
3. current = stored registration.
4. requested.events = none.
5. requested.trigger_mode = current.trigger_mode.
6. Perform the transactional transition.
7. Applied:
     - erase fd from _registered;
     - return true.
8. FailedRolledBack:
     - retain _registered unchanged;
     - return false.
9. RollbackFailed:
     - mark backend unhealthy;
     - return false.
```

### 8.8 Why same-mask mode changes must perform native work

The following is no longer a no-op:

```text
current:
    events = Read | Write
    trigger = Edge

requested:
    events = Read | Write
    trigger = Level
```

Both enabled filters must be re-added without `EV_CLEAR`.

Likewise Level→Edge must re-add both with `EV_CLEAR`.

The existing kqueue test asserting "unchanged mask causes no operation" must be narrowed to:

> unchanged event mask **and unchanged trigger mode** cause no native filter operation.

---

## 9. Fake EventLoop backend

Update `test/support/fake_event_loop_backend.hpp`.

### 9.1 Signature

Implement:

```cpp
auto add_fd(int fd,
            FdEvents events,
            FdTriggerMode trigger_mode) -> bool override;
```

### 9.2 Recorded state

At minimum retain:

```cpp
int           last_added_fd{-1};
FdEvents      last_added_events;
FdTriggerMode last_added_trigger_mode{FdTriggerMode::Edge};
```

Prefer also recording complete calls:

```cpp
struct FakeFdRegistration {
    int           fd;
    FdEvents      events;
    FdTriggerMode trigger_mode;
};

std::vector<FakeFdRegistration> add_fd_history;
```

This makes replacement tests less dependent on one "last call" field.

Existing result queues/failure injection remain unchanged.

### 9.3 EventLoop test access

Extend `EventLoopTestAccess` only as needed to verify retained public registration after a failed replacement.

A suitable helper is:

```cpp
static auto fd_trigger_mode(EventLoop const& loop, int fd)
    -> std::optional<FdTriggerMode>;
```

or a private-test registration snapshot containing both events and mode.

Do not expose registration inspection through the public EventLoop API.

---

## 10. Existing production watcher migration

Run:

```sh
rg -n '\bwatch_fd\s*\(' src test
```

after changing the signature and classify every caller.

The known production callers at the Stage 5.4 baseline are:

### 10.1 `src/net/tcp_socket.cpp`

Every watch created by `TcpSocket::update_watch()` uses:

```cpp
FdTriggerMode::Edge
```

`TcpSocket` already satisfies the contract:

- reads loop until would-block;
- writes loop until would-block or the output buffer is drained;
- read/write interest is changed when necessary.

No socket algorithm change is required.

### 10.2 `src/net/local_socket_linux.cpp`

Every `LocalSocket` watch uses:

```cpp
FdTriggerMode::Edge
```

The implementation already drains native I/O according to the existing hidden edge-trigger assumption and disables/re-enables readiness interest when its bounded input buffer requires it.

Do not convert it to level mode.

### 10.3 `src/net/local_socket_macos.cpp`

Use:

```cpp
FdTriggerMode::Edge
```

for the same reasons as Linux.

The two platform implementations should remain behaviorally aligned.

### 10.4 `src/net/local_server_linux.cpp`

The listener watch uses:

```cpp
FdTriggerMode::Edge
```

The accept callback already accepts repeatedly until would-block or the configured pending-connection limit causes the listener watch to be removed.

### 10.5 `src/net/local_server_macos.cpp`

The listener watch also uses:

```cpp
FdTriggerMode::Edge
```

It likewise drains available accepts until would-block or intentionally pauses acceptance.

### 10.6 Other production callers

Any additional production caller found by the required `rg` audit must be classified based on ownership:

- code that directly owns and drains nonblocking native I/O should normally use `Edge`;
- adapters that only pass readiness to another I/O engine and cannot prove drain-to-would-block behavior require `Level`.

Do not mechanically choose Edge merely to preserve compilation.

If a caller cannot be classified from its implementation, stop and document it rather than guessing.

---

## 11. CurlMultiAdapter migration

### 11.1 Watch registration

In:

```text
src/net/http/curl_multi_priv.cpp
```

all libcurl socket watches use:

```cpp
FdTriggerMode::Level
```

Example shape:

```cpp
auto handle = _loop.watch_fd(
    fd,
    update.events,
    jb::core::FdTriggerMode::Level,
    [callback](int ready_fd, jb::core::FdEvents events) -> void {
        ...
    });
```

The curl adapter must not use edge mode.

### 11.2 Remove Stage 5.4 refresh workaround

Delete:

```cpp
CurlMultiAdapter::refresh_socket_watch_after_readiness()
```

and:

```cpp
CurlMultiAdapterTestAccess::refresh_socket_watch_after_readiness()
```

Delete their declarations and focused tests.

Restore `handle_socket_ready()` to the ordinary readiness-drive behavior:

```text
1. verify adapter availability;
2. mark socket callback active;
3. drive curl for the reported fd/events;
4. clear socket-callback-active state;
5. do not queue an artificial same-watch replacement.
```

No replacement is scheduled merely because libcurl retained the same interest after the drive.

### 11.3 Preserve real reconciliation

Do **not** remove the existing reconciliation mechanism.

It remains required for:

- socket interest changes issued from curl callbacks;
- socket removal;
- timer updates;
- callback reentrancy;
- safe deferred replacement/removal outside the currently executing callback;
- backend failure handling.

Only the synthetic "refresh unchanged watch after readiness" path is removed.

### 11.4 Callback replacement semantics

If libcurl itself requests the same event mask again, reconciliation may still install a new callback state.

Because EventLoop now recognizes unchanged `{events, Level}` native state, this becomes a callback-only replacement and causes no epoll/kqueue operation.

That behavior is portable and is not a rearm.

---

## 12. EventLoop unit coverage

Expand `test/event-loop-test.cpp`.

Existing direct `watch_fd()` calls use explicit `FdTriggerMode::Edge` unless the test specifically exercises level behavior.

### 12.1 Validation

Retain existing validation tests and verify that a valid mode reaches the fake backend.

There is no invalid enum value contract to test.

### 12.2 Registration propagation

Verify:

```text
watch_fd(fd, Read, Edge, callback)
```

records Edge in the fake backend.

Verify separately for Level.

### 12.3 Callback-only replacement

Freeze the new cross-platform rule:

```text
1. Install Read/Edge callback A.
2. Record backend add count.
3. Replace fd with Read/Edge callback B.
4. Assert backend add count did not change.
5. Inject readiness.
6. Assert only callback B runs.
```

Repeat or table-drive for Level.

### 12.4 Event-mask replacement

Verify:

```text
Read/Edge -> Write/Edge
```

does call the backend.

If the backend rejects it:

- the old callback remains;
- the old event mask remains;
- the old trigger mode remains.

### 12.5 Trigger-mode replacement

Verify:

```text
Read/Edge -> Read/Level
```

calls the backend even though the mask is unchanged.

Verify the reverse transition too.

### 12.6 Trigger replacement failure

Example:

```text
1. Install Read/Edge callback A.
2. Make backend add fail.
3. Attempt Read/Level callback B.
4. watch_fd() returns invalid.
5. Stored public registration remains Read/Edge + callback A.
6. Inject Read readiness.
7. callback A runs; callback B does not.
```

### 12.7 Combined replacement

Exercise at least:

```text
Read/Edge -> Write/Level
```

to prove mask and mode are replaced as one public registration.

### 12.8 Removal

Existing removal tests remain valid. Add no special mode-dependent removal behavior.

---

## 13. Native EventLoop readiness tests

Add deterministic native behavior coverage using a nonblocking pipe or socketpair.

Tests must avoid sleeps as correctness synchronization.

### 13.1 Edge readiness repeats only after a new transition

Required scenario:

```text
1. Create nonblocking pipe/socketpair.
2. Watch readable fd with Edge.
3. Write one byte.
4. process_events(Watchers, 0).
5. callback count == 1.
6. Do not consume the byte.
7. process_events(Watchers, 0) again.
8. callback count remains 1.
9. Drain the byte to would-block.
10. Write another byte.
11. process events.
12. callback count == 2.
```

This test should run against the actual platform backend, not the fake.

### 13.2 Level readiness repeats while state remains ready

Required scenario:

```text
1. Create nonblocking pipe/socketpair.
2. Watch readable fd with Level.
3. Write one byte.
4. process events.
5. callback count == 1.
6. Leave the byte unread.
7. process events again.
8. callback count == 2.
9. Drain the byte.
10. process events again with timeout 0.
11. callback count remains 2.
```

### 13.3 Runtime mode transition

Prove the native registration actually changes modes.

One deterministic sequence is:

```text
1. Start Level.
2. Make fd readable.
3. Prove repeated readiness.
4. Drain it.
5. Replace watch with Edge.
6. Make fd readable again.
7. Prove one notification only while unread.
8. Drain it.
9. Replace with Level.
10. Make fd readable.
11. Prove repeated readiness again.
```

Do not make the test depend on whether changing mode immediately reports readiness that was already pending at the exact time of replacement. The public API need not promise that timing detail.

The test should prove the behavior of subsequent readiness states.

---

## 14. kqueue transition coverage

Expand `test/kqueue-backend-test.cpp`.

Update the scripted callback to record `KqueueFilterMode` rather than only `enable=true/false`.

At minimum cover:

### 14.1 Initial edge registration

```text
none -> Read|Write / Edge
```

Expected:

```text
Read  -> Edge
Write -> Edge
```

### 14.2 Initial level registration

```text
none -> Read|Write / Level
```

Expected:

```text
Read  -> Level
Write -> Level
```

### 14.3 Completely unchanged registration

```text
Read|Write / Edge -> Read|Write / Edge
```

Expected:

```text
no operations
```

Repeat for Level if useful.

### 14.4 Same mask Edge→Level

```text
Read|Write / Edge -> Read|Write / Level
```

Expected:

```text
Read  -> Level
Write -> Level
```

### 14.5 Same mask Level→Edge

Expected both enabled filters to be reapplied as Edge.

### 14.6 Add one filter without changing mode

Example:

```text
Read / Edge -> Read|Write / Edge
```

Only Write is changed.

### 14.7 Remove one filter without changing mode

Example:

```text
Read|Write / Edge -> Read / Edge
```

Only Write becomes Disabled.

### 14.8 Mask and mode change together

Example:

```text
Read / Edge -> Write / Level
```

Expected:

```text
Read  -> Disabled
Write -> Level
```

### 14.9 Partial failure rollback after mode change

Example:

```text
Read|Write / Edge -> Read|Write / Level
```

with:

```text
Read -> Level succeeds
Write -> Level fails
Read -> Edge rollback succeeds
```

Result:

```text
FailedRolledBack
```

### 14.10 Partial failure rollback after mixed transition

Exercise a sequence containing both disable and mode/add operations.

### 14.11 Rollback failure

Example:

```text
first change succeeds
second change fails
rollback of first change fails
```

Result:

```text
RollbackFailed
```

`KqueueBackend` must then become unhealthy.

---

## 15. Existing networking regressions

Run unchanged socket/server suites after converting their calls to explicit Edge mode.

Required focused suites include:

```text
tcp-socket-test
local-socket-test
local-server-test
local-rpc-test
rpc-client-test
rpc-server-test
```

where available on the current platform.

The goal is to prove that making edge semantics explicit does not change existing behavior.

Pay special attention to:

- partial writes;
- buffered reads;
- read-buffer-limit disable/re-enable;
- listener pending-connection pause/resume;
- connect completion;
- peer close;
- callback replacement;
- watch registration/removal failure.

No production socket algorithm should need modification merely because the trigger argument becomes explicit.

If an existing socket test fails because it was accidentally relying on callback-only epoll `MOD` as a rearm, treat that as another hidden edge-contract bug and fix the consumer to satisfy edge semantics. Do not restore rearm side effects.

---

## 16. Stage 5.4 HTTP regression coverage

### 16.1 Preserve the discovered sustained upload case

The exact Stage 5.4 regression remains mandatory:

```text
POST body size: 1,048,577 bytes
```

The transfer must complete without:

- polling;
- sleeps;
- a runner thread;
- explicit watch refresh;
- repeated artificial curl drive calls.

The test should pass because the curl fd is now level-triggered.

### 16.2 Large download

Add or strengthen a comparable sustained-response case large enough to require multiple curl readiness drives.

The server should write/drain through its existing controlled loopback fixture and the client must receive the full response while respecting Stage 5.4 capture behavior.

The case should exercise level-triggered read readiness, complementing the upload's write-readiness regression.

### 16.3 Repeated focused runs

Repeat the sustained upload and download scenarios enough times to catch lost-readiness races.

Use the existing deterministic/bounded test harness. Do not introduce arbitrary sleep-based synchronization.

### 16.4 Existing HTTP matrix

Re-run all Stage 5.3/5.4 system-client tests, including:

- basic GET;
- concurrent transfers;
- cancellation;
- methods;
- request bodies;
- headers;
- status/header parsing;
- chunked responses;
- decompression;
- capture limits;
- keep-alive reuse;
- backend failure injection.

The EventLoop change must not alter HTTP completion or callback identity semantics.

---

## 17. macOS requirement

This stage directly changes the kqueue implementation because the Stage 5.4 workaround is known not to work there.

Therefore Stage 5.4a should not be considered completely cross-platform evidenced from Linux-only execution.

### Required code-level proof on every platform

Portable deterministic tests must cover:

- kqueue transition planning;
- rollback behavior;
- EventLoop public replacement semantics.

### Required real Linux proof

Run:

- native edge/level EventLoop tests;
- existing networking regressions;
- full Stage 5.4 HTTP suite;
- sustained upload/download repeatedly.

### Required real macOS proof before declaring the deficit resolved

On a real macOS build using the kqueue backend, run at minimum:

```text
event-loop-test
kqueue-backend-test
tcp-socket-test
local-socket-test
local-server-test
system-http-client-test
```

and specifically confirm:

- native Edge behavior;
- native Level behavior;
- Edge→Level and Level→Edge replacement behavior;
- the 1,048,577-byte sustained upload;
- the large sustained response.

This focused macOS gate does not pull the complete optional Phase 5.17/5.18 macOS scope forward. Redirect/TLS/proxy/daemon features that have not yet been implemented remain in their planned stages.

It only verifies the kqueue behavior introduced by Stage 5.4a.

If real macOS execution is not available during the implementation PR, the handoff must explicitly say that kqueue native verification remains outstanding and the change must not be described as proven cross-platform.

---

## 18. Public-header and documentation gate

### 18.1 Doxygen

Update all changed public declarations in:

```text
src/core/event_loop_types.hpp
src/core/event_loop.hpp
```

The comments must explain:

- Edge;
- Level;
- drain-to-would-block responsibility;
- repeated level callbacks;
- explicit mode selection;
- replacement behavior;
- backend failure preservation;
- owner-thread restriction.

### 18.2 First-include coverage

The current repository has no dedicated EventLoop public-header boundary target.

Add:

```text
test/core-event-loop-public-header-test.cpp
```

with `event_loop.hpp` as its first include and compile-time/basic usage of `FdTriggerMode`.

Add the corresponding target to `test/CMakeLists.txt`.

If desired, `event_loop_types.hpp` can receive its own first-include boundary test, but at minimum the EventLoop public header must compile independently and expose the enum transitively as designed.

### 18.3 Comments

Add implementation comments only where rationale is non-obvious:

- why callback-only replacement intentionally skips the backend;
- why kqueue re-adds enabled filters for a mode change;
- why rollback failure invalidates the kqueue backend;
- why curl uses Level while project-owned sockets use Edge.

Do not narrate direct enum-to-flag mappings where the code is already self-explanatory.

---

## 19. Implementation sequence

Stage 5.4a remains one review boundary, but Codex should implement it internally in this order.

### Step 1 — Freeze public readiness semantics

Change:

```text
event_loop_types.hpp
event_loop.hpp
event_loop.cpp
event_loop_backend.hpp
```

Add required `FdTriggerMode` and callback-only replacement behavior.

Update fake backend and EventLoop tests enough for the core contract to compile.

Do not add temporary compatibility overloads.

### Step 2 — Implement epoll modes

Update epoll registration mapping.

Run focused EventLoop tests on Linux before touching curl.

### Step 3 — Implement transactional kqueue modes

Refactor kqueue registration bookkeeping and transition helper.

Expand `kqueue-backend-test.cpp`.

The helper tests must compile and pass on Linux as deterministic pure logic even though the native backend translation unit is not built there.

### Step 4 — Migrate all edge consumers

Use:

```sh
rg -n '\bwatch_fd\s*\(' src test
```

and convert all existing EventLoop consumers deliberately.

Production socket/server users remain Edge.

Build the full tree at this point. Any unconverted call is a compile failure.

### Step 5 — Move curl to Level

Change curl socket registration to Level.

Remove all unchanged-watch refresh code and test seams.

Run focused HTTP client tests.

### Step 6 — Add native behavioral and sustained-transfer proof

Add the Edge/Level EventLoop behavior tests plus large upload/download HTTP coverage.

Run Linux and focused macOS verification.

Only after these pass is Stage 5.4a complete.

---

## 20. Verification

### 20.1 Static checks

For every changed C++ translation unit/header:

- clangd diagnostics are clean;
- formatting is clean;
- `git diff --check` is clean.

Audit:

```sh
rg -n '\bwatch_fd\s*\(' src test
```

Every result must contain an explicit trigger mode.

Audit that the Stage 5.4 workaround is gone:

```sh
rg -n 'refresh_socket_watch_after_readiness|same-watch refresh|unchanged.*watch.*refresh' src test
```

Expected result: no implementation/test dependency on the workaround.

Do not flag legitimate design/history documentation unless it incorrectly describes current behavior.

### 20.2 Focused Linux build/tests

At minimum build and run:

```text
core-event-loop-public-header-test
event-loop-test
kqueue-backend-test
tcp-socket-test
local-socket-test
local-server-test
local-rpc-test
system-http-client-test
```

plus affected RPC/socket tests discovered through the build.

### 20.3 Full Linux regression

After focused tests:

```sh
cmake --build .bld
ctest --test-dir .bld/test --output-on-failure
```

The public API change affects low-level infrastructure broadly enough that a full existing test-suite run is required before Stage 5.5.

A separate SQLite-disabled build/test cycle is not required specifically for Stage 5.4a; the Phase 5 final SQLite-disabled boundary remains in Stage 5.19.

### 20.4 macOS focused verification

Build the changed core/net/net-http targets and run the focused suites from section 17.

Record:

- macOS version;
- Apple Clang version;
- libcurl source/version;
- exact focused test result;
- repeated sustained-transfer result.

---

## 21. Failure handling during implementation

Stop and revise this design rather than silently widening the stage if any of these occur:

- a current `watch_fd()` production caller cannot safely be classified as Edge or Level;
- epoll or kqueue cannot provide the documented Level semantics with the proposed native mappings;
- changing kqueue `EV_CLEAR` state on an existing filter requires delete/re-add rather than documented `EV_ADD` modification;
- callback-only backend suppression breaks a consumer that was intentionally relying on epoll `EPOLL_CTL_MOD`;
- libcurl still stalls under true Level registration;
- level registration produces an unavoidable busy loop under ordinary curl operation;
- kqueue rollback cannot preserve EventLoop's transactional registration contract;
- fixing the deficit requires changing public HTTP semantics.

Such a finding is architectural evidence, not permission to add another platform-specific workaround.

---

## 22. Acceptance checklist

- [ ] `FdTriggerMode` exists publicly with `Edge` and `Level`.
- [ ] `watch_fd()` requires the mode; there is no compatibility overload or default.
- [ ] Public Doxygen fully describes both readiness contracts.
- [ ] EventLoop stores callback, event mask, and trigger mode together.
- [ ] Callback-only replacement performs no backend operation.
- [ ] Event-mask or trigger-mode replacement is transactional.
- [ ] epoll Edge uses `EPOLLET`.
- [ ] epoll Level omits `EPOLLET`.
- [ ] kqueue Edge uses `EV_CLEAR`.
- [ ] kqueue Level omits `EV_CLEAR`.
- [ ] kqueue same-mask Edge↔Level performs native filter modifications.
- [ ] kqueue unchanged mask+mode remains a native no-op.
- [ ] kqueue partial failures roll back successfully or mark the backend unusable.
- [ ] Fake EventLoop backend records trigger mode.
- [ ] Native EventLoop tests distinguish Edge from Level.
- [ ] Native tests cover mode replacement.
- [ ] Every production/test `watch_fd()` call explicitly selects a mode.
- [ ] `TcpSocket` uses Edge.
- [ ] Linux and macOS `LocalSocket` use Edge.
- [ ] Linux and macOS `LocalServer` use Edge.
- [ ] `CurlMultiAdapter` uses Level.
- [ ] `refresh_socket_watch_after_readiness()` is deleted.
- [ ] Curl does not emulate level triggering with artificial re-registration or repeated task-loop drives.
- [ ] 1,048,577-byte sustained upload completes.
- [ ] Large sustained response completes.
- [ ] Existing Stage 5.3/5.4 HTTP tests remain green.
- [ ] Existing socket/server/RPC regressions remain green.
- [ ] EventLoop public-header boundary test passes.
- [ ] Full Linux CTest suite passes.
- [ ] Focused real macOS kqueue verification passes, or the handoff explicitly records it as outstanding.
- [ ] clangd, formatting, and `git diff --check` are clean.

---

## 23. Phase 5 plan impact

Stage 5.4a is inserted between the existing Stage 5.4 and Stage 5.5.

The effective sequence becomes:

```text
5.4   Complete methods, headers, bodies, responses, and decompression
5.4a  Make EventLoop FD readiness semantics explicit
5.5   Complete timeout, cancellation, and error mapping
...
```

Stage 5.4's functional HTTP data-path work remains valid.

The following Stage 5.4 implementation detail is superseded:

```text
refresh an unchanged EventLoop watch after curl readiness
```

After 5.4a the required architecture is:

```text
project-owned socket I/O
    -> EventLoop Edge watch
    -> consumer drains native operation to would-block

libcurl-managed socket I/O
    -> EventLoop Level watch
    -> one curl_multi_socket_action() drive per readiness callback
    -> EventLoop reports readiness again if the condition remains true
```

Stages 5.5 through 5.19 otherwise retain their existing scope and ordering.

The optional later macOS Phase 5 stages remain responsible for full HTTP/TLS/scheduler/daemon macOS completion. Stage 5.4a only pulls forward enough real macOS validation to prove the new kqueue readiness primitive itself.

---

## 24. Handoff requirement

At completion, the Stage 5.4a handoff must record:

- exact final revision;
- files changed;
- final `watch_fd()` signature;
- all production Edge callers;
- all production Level callers;
- epoll mapping;
- kqueue mapping;
- confirmation that callback-only replacement performs no native operation;
- confirmation that the Stage 5.4 curl refresh workaround was removed;
- Linux focused/full test results;
- repeated sustained upload/download results;
- macOS focused result or explicit outstanding status;
- any deviations from this design.

Stage 5.5 may start only after that handoff is reviewed and accepted.

Suggested commit subject:

```text
add explicit EventLoop fd trigger modes
```