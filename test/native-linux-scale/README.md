# Stage 9.27 Linux scale and fault gate

`jobu-scale-test` runs bounded deterministic regressions in ordinary CTest.
`JOBU_SCALE_PROFILE=medium` selects 100 queues, 1,000 pending and 10,000 terminal
runs; `full` selects 100 queues, 10,000 pending and 100,000 terminal runs. Half the
pending work and one third of history concentrate in the first queue. CLI/HTTP,
weights, inherited/finite/unlimited retention and timing qualities are mixed.
The full profiles are explicit measurements, rather than ordinary CTest work.

Full-size data is used by backlog/telemetry, retention/statistics and the exported
native seed. Repeated-runner fairness uses 100 queues and 1,000 pending runs with
no terminal history. The transaction-fault matrix uses 3 queues, 420 pending and
300 terminal runs; cascade and owner regressions use smaller focused datasets.
Each measurement reports its own work size.

```sh
cmake --build .bld --target jobu-scale-test
ctest --test-dir .bld/test -R '^jobu-scale-test$' --output-on-failure
JOBU_SCALE_PROFILE=full .bld/test/jobu-scale-test '[scale]'
python3 -B test/native-linux-scale/harness_test.py
```

Each `SCALE_MEASUREMENT` line records one real operation duration and its work
size. `SCALE_PLAN` records production statement plans. SQLite cache, retained
fixture data and database-driver observation overhead remain part of the process;
the observer retains unique SQL templates/counters rather than all calls or row
payloads. Report sample counts and measured costs, without a universal latency SLA.
Checkpoint/retention page yields are deterministic; one scheduler cycle and an
affected queue mutation can scan multiple pages synchronously.

The native gate is opt-in and requires an approved disposable systemd VM, existing
project dependencies, Python 3.12+, root access and the dedicated non-root `jobu`
identity. It installs the reviewed custom `/opt/jobu926` Runtime package and starts
a separate, temporary `jobu927-scale.service`. It refuses existing destinations.
It does not install dependencies, enable boot services or restore a VM snapshot.

Export an immutable snapshot with read-only `git archive`, recording its checksum
and the exact jj snapshot. `produce.sh SOURCE_TAR SNAPSHOT FRESH_DIRECTORY` builds
the Release test helper and fresh custom-profile Runtime packages from that same
source on the native platform. The helper remains outside packages. Previously
retained packages can be reused only when their product sources are unchanged.
For controller-only corrections, record its separate revision; the evidence also
hashes the executed controller file. Product/helper provenance remains the
unchanged producer snapshot.

```sh
sudo python3 -B verify.py --disposable-vm \
    --runtime /absolute/custom-runtime.tar.gz --runtime-sha256 SHA256 \
    --product-revision PRODUCT_REVISION --source-snapshot SUPPORT_SNAPSHOT \
    --controller-revision CONTROLLER_REVISION \
    --helper /var/tmp/jobu927-producer/artifacts/jobu-scale-test \
    --fixture /var/tmp/jobu927-fixture --evidence /var/tmp/jobu927-evidence
```

The gate runs both native scale profiles, validates the full fixture through real
startup recovery, and kills a native service-helper subprocess after an actual
checkpoint/purge write or acknowledged commit. The helper flushes a PID-bearing
milestone and stops itself; the controller verifies its parent, identity and
stopped state before SIGKILL. Restart uses production recovery and verifies lost
open tails, persisted lower bounds, atomic run/timing/attempt deletion and repeat
recovery. This is synchronized native SQLite/service evidence; the killed helper
is explicitly distinct from the installed daemon. The latter separately supplies
native RPC responsiveness, warning-tail, checkpoint-tail, retained-statistics and
healthy systemd stop observations at the same full data size.

The closed-seed check rejects an existing WAL before opening the database, then
uses immutable read mode to avoid creating sidecars during inspection. This mode
is confined to the privately owned closed seed. Live daemon observations retain
ordinary read-only WAL access.
Fixture database copies are set to mode 0600 before service-account ownership
handoff, independently of the controller's inherited umask.

Evidence includes commands, exits, logs, every measurement, native environment,
package/helper hashes, service credentials/RSS, journal and teardown. The gate
stops on failure. Preserve the evidence and diagnose before approving a narrow
correction. Normal return removes only its unit, installed prefix and fixture
roots, preserving `jobu` and prior Stage 9.26 evidence. Killing the outer harness
cannot promise cleanup; its registered paths and events remain the recovery guide.
