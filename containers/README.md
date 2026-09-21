# Containerized Linux builds

The `tools/container` wrapper provides reproducible Ubuntu 24.04 and Alpine
3.22 development environments. It builds images from the Dockerfiles in this
directory and keeps each environment's incremental build tree under the
repository root.

## Use the wrapper

Build an image once, then configure, build, and test with it:

```sh
tools/container image ubuntu
tools/container configure ubuntu
tools/container build ubuntu
tools/container test ubuntu

tools/container image alpine
tools/container configure alpine
tools/container build alpine
tools/container test alpine
```

The `ubuntu` and `alpine` aliases select Ubuntu 24.04 and Alpine 3.22. The
fully qualified `ubuntu-24.04` and `alpine-3.22` names are also accepted.

Use `run` for an arbitrary command or `debug` when a debugger or tracer needs
`SYS_PTRACE`:

```sh
tools/container run alpine cmake --build .bld-alpine-322 --target process-linux-test
tools/container debug alpine gdb --args .bld-alpine-322/test/process-linux-test
```

## Working-directory behavior

The wrapper mounts the source tree read-only at its existing absolute path and
mounts only the selected build tree read-write:

- Ubuntu: `.bld-ubuntu-2404`
- Alpine: `.bld-alpine-322`

Containers run with the invoking user's UID and GID, so persistent build output
remains owned by that user. The stable absolute path also keeps generated
`compile_commands.json` entries usable by host tools.

The wrapper intentionally does not add an init process, preserving container
PID 1 behavior relevant to Linux process tests. Rebuild an image when its
Dockerfile changes, and recreate that environment's build tree after material
compiler or system-library changes.
