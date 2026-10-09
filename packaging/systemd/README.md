# Systemd template source

`jobud.service.in` is the inactive source template for the Linux system profile.
It contains unsubstituted placeholders and is not ready to install into systemd.
Its directory directives require the explicit `/run/jobu` and `/var/lib/jobu`
operational paths. Profile generation, systemd-specific escaping, account setup
and native validation belong to Stage 9.25 and its deployment gate.

CMake installs these files only as data under `share/jobu/services`. It does
not create the `jobu` identity, copy active configuration, install a vendor unit,
reload systemd, enable a service or start a daemon. Native RPC readiness and
process cleanup require separate deployment verification.
