# JobU systemd setup

Select a profile while configuring a fresh Linux build with the SQLite driver:

| `JB_JOBU_SYSTEMD_PROFILE` | Assets and operational layout |
| --- | --- |
| `none` (default) | Template source, this guide and readiness probe only; no generated unit. Generic operational defaults remain unchanged. |
| `system` | Prefix `/usr`, config `/etc/jobu`, state `/var/lib/jobu`, runtime `/run/jobu`; generated unit in the service data directory and vendor directory `/usr/lib/systemd/system`. |
| `custom` | Generated unit in the service data directory, using configured binary/config paths. Protected state/runtime directories are provisioned by the administrator. |

Both generated profiles start directly as `User=jobu`, `Group=jobu`, with
`UMask=0077`, `NoNewPrivileges=yes`, no core dumps and journal output. They use
`Type=exec`, failure restart with a five-second delay and start limiting, and
SIGTERM followed by systemd's final cgroup cleanup after a 15-second stop timeout
(`KillMode=mixed`). Arbitrary CLI jobs and HTTP calls remain available; additional
filesystem/home/network restrictions require review against the intended jobs.
Do not assume cleanup of deliberately escaped sessions is established.

The system profile has `StateDirectory=jobu` and `RuntimeDirectory=jobu`, both
mode 0700. Conflicting operational paths or a prefix other than `/usr` fail
configuration. Custom units omit these directives and require protected,
explicitly provisioned directories matching the generated INI. All profiles
require an explicit active config through `--config`; no root-drop setting is
needed for a service already started as `jobu`.

`jobud.service.in` is source data with placeholders. Use the generated
`jobud.service`, which quotes/escapes systemd command arguments. Installation
and TGZ extraction never create an account, active config, state/runtime data,
reload the service manager, enable/start or restart a daemon. Selecting `system`
installs the vendor unit file; activation remains a separate administrator action.

## System layout

Build/package with the installation guide's prerequisites and a fresh directory:

```sh
cmake -S . -B .bld-system -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DJB_JOBU_SYSTEMD_PROFILE=system \
    -DBUILD_TESTING=OFF -DJB_BUILD_EXAMPLES=OFF -DJB_PACKAGE_REMAP_PATHS=ON
cmake --build .bld-system --parallel
DESTDIR=/tmp/jobu-stage cmake --install .bld-system
```

Inspect the staged files before installing on the deployment host. DESTDIR and
an install-time `--prefix` do not rewrite compiled paths, the INI or the unit.
Switching profiles in an existing build retains operational cache values; use a
fresh build or explicitly reset all three `JB_JOBU_*DIR` values. The system
profile initializes fresh values to its required paths.

After installing the reviewed Runtime files, provision a dedicated system group
and user. On Ubuntu, these commands create missing identities only:

```sh
getent group jobu >/dev/null || sudo groupadd --system jobu
getent passwd jobu >/dev/null || sudo useradd --system --gid jobu \
    --home-dir /var/lib/jobu --no-create-home --shell /usr/sbin/nologin jobu
id jobu
sudo passwd -S jobu
```

Verify that an existing `jobu` identity is a dedicated non-root service account
with primary group `jobu`, no login shell and a locked password. Do not repurpose
an unrelated account or assume fixed numeric IDs. Account tools and nologin paths
vary by distribution; use their native equivalents. New system accounts created
by the Ubuntu command have locked passwords and no state/home directory.

Create the config parent and copy the example **only for initial setup**. The
guards below preserve an existing active file or symlink:

```sh
sudo install -d -o root -g root -m 0755 /etc/jobu
sudo test ! -e /etc/jobu/jobud.ini && sudo test ! -L /etc/jobu/jobud.ini && \
    sudo install -o root -g jobu -m 0640 \
        /usr/share/jobu/jobud.ini.example /etc/jobu/jobud.ini
sudo -u jobu /usr/bin/jobud --check-config --config /etc/jobu/jobud.ini
systemd-analyze --generators=no --man=no --recursive-errors=yes verify \
    /usr/lib/systemd/system/jobud.service
```

Review the full example, retaining its generated database/socket paths. Its unsafe
root permissions are false; optional account/proxy/CA and attribute overrides
remain commented out. Root-owned, group-readable mode 0640 lets `jobu` read the
config without permitting the service account to change it. Protect every
controlling parent against replacement. Check mode creates no daemon resources
and does not promise later access or RPC readiness.

Only after reviewing these checks, activate deliberately:

```sh
sudo systemctl daemon-reload
sudo systemctl start jobud.service
sudo -u jobu sh /usr/share/jobu/services/wait-ready.sh \
    /usr/bin/jobuctl /run/jobu/jobud.sock
systemctl status jobud.service --no-pager
sudo journalctl -u jobud.service -n 50 --no-pager
# Optional boot activation after successful setup:
sudo systemctl enable jobud.service
# Deliberate stop:
sudo systemctl stop jobud.service
```

The probe queries `system.info` with an explicit socket and CLI overall deadline.
Defaults are 15 attempts, 1000 ms per RPC and a one-second pause between attempts
(up to 29 seconds of deadline/pause budget, plus process scheduling overhead).
Optional arguments set attempts (1..300) and timeout milliseconds (1..60000),
without leading zeros. It prints the successful JSON result, or exits nonzero
after its finite retry budget with the last diagnostic. `Type=exec` and
`systemctl is-active` alone do not establish RPC readiness. The probe runs as the
socket owner; it does not read daemon config or search for other endpoints.

Stop deliberately before replacing binaries. Compare the new example with active
configuration, apply selected changes, run the config check and restart explicitly.
Older database formats are rejected without migration or deletion. Do not remove
a lock pathname or add an unconditional pre-start unlink to force startup.

## Administrative socket group

The default socket is owner-only (0600). Mode 0660 grants **full daemon authority**
to the selected group, including CLI job creation and secret management; it is
not read-only monitoring or RBAC.

For the system profile, use primary group `jobu`: set `socket.mode = 0660` and
`socket.group = jobu` in active config, and add a reviewed service drop-in:

```ini
[Service]
RuntimeDirectoryMode=0750
```

Stop before applying the change, then reload/check/restart and probe as above.
The runtime directory must grant matching group traversal, retain final-user
ownership and have no group/world write access. Provision only deliberately
trusted administrators as group members and refresh their login group set.
For a different supplementary socket group, use the custom profile with matching
directory ownership and service supplementary groups; review both together.

## Custom prefix

For example, configure a fresh build with `CMAKE_INSTALL_PREFIX=/opt/jobu` and
`JB_JOBU_SYSTEMD_PROFILE=custom`. Defaults are then `/opt/jobu/etc`,
`/opt/jobu/var/lib` and `/opt/jobu/var/run`. Explicit `JB_JOBU_*DIR` overrides are
available. Keep the UNIX socket path short enough for the native limit.

Install the Runtime files and provision the dedicated identity as above. Provision
root-owned, non-group/world-writable parents (including `/opt/jobu/var`) and
final-user-owned state/runtime leaves, normally mode 0700. For a new dedicated
prefix, the directory steps are:

```sh
sudo install -d -o root -g root -m 0755 /opt/jobu/etc /opt/jobu/var
sudo install -d -o jobu -g jobu -m 0700 /opt/jobu/var/lib /opt/jobu/var/run
```

Review existing ownership first; do not recursively repair an existing tree.
Secure an initial `/opt/jobu/etc/jobud.ini` from the generated example with the
same no-overwrite guards and root:jobu 0640 mode. Check it as `jobu` using
`/opt/jobu/bin/jobud`. Review and copy the generated
`/opt/jobu/share/jobu/services/jobud.service` to `/etc/systemd/system/jobud.service`
as root:root mode 0644, preserving any existing administrator unit until reviewed.
Verify that unit, then reload/start/probe deliberately. The readiness command uses
`/opt/jobu/bin/jobuctl` and the configured socket explicitly. Prefixes containing
spaces require normal shell quoting in these administrator commands; the generated
unit handles its separate systemd quoting automatically.
Install prefixes/binary/data destinations containing quotes, backslashes or CMake
variable expressions are rejected because generated CMake install scripts cannot
preserve them. Operational config paths support quoted/backslash characters.
Executable destinations additionally reject single quotes and dollar signs due
to systemd's separate executable-path parsing. Spaces and percent signs are supported.

Syntax/profile checks do not establish native managed-service acceptance. The
Stage 9.26 gate separately verifies readiness, permissions, permanent root drop,
independent root CLI policy, restart/collisions and child cleanup using installed
package binaries. A verification fixture with inert executable placeholders proves
unit parsing only, not executable compatibility or deployment.
