# Containerized Linux builds

The `tools/container` wrapper provides versioned Ubuntu and Alpine development
environments. It builds images from one shared Dockerfile per distribution and
keeps each version's incremental build tree under the repository root.

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

The default aliases select Ubuntu 26.04 and Alpine 3.24. Defaults advance through
explicit wrapper changes; they do not follow Docker's `latest` tags.

| Environment aliases | Image tag | Build directory |
| --- | --- | --- |
| `ubuntu`, `ubuntu-2604`, `ubuntu-26.04` | `deferra-dev:ubuntu-26.04` | `.bld-ubuntu-2604` |
| `ubuntu-2404`, `ubuntu-24.04` | `deferra-dev:ubuntu-24.04` | `.bld-ubuntu-2404` |
| `alpine`, `alpine-324`, `alpine-3.24` | `deferra-dev:alpine-3.24` | `.bld-alpine-324` |
| `alpine-323`, `alpine-3.23` | `deferra-dev:alpine-3.23` | `.bld-alpine-323` |
| `alpine-322`, `alpine-3.22` | `deferra-dev:alpine-3.22` | `.bld-alpine-322` |

Use a versioned alias with any action to select an older environment explicitly:

```sh
tools/container image alpine-322
tools/container configure alpine-322
tools/container build alpine-322 --parallel 2
tools/container test alpine-322
```

Existing Ubuntu 24.04 and Alpine 3.22 images and build trees remain usable through
their versioned aliases. Configure the new default environments before building
them; build trees are not migrated between versions. The wrapper supplies
`BASE_VERSION` to the shared Dockerfile, keeping the selected base version and
image tag aligned. Rebuilding can pick up newer patches and distribution packages
within that release.

Use `run` for an arbitrary command or `debug` when a debugger or tracer needs
`SYS_PTRACE`:

```sh
tools/container run alpine cmake --build .bld-alpine-324 --target process-linux-test
tools/container debug alpine gdb --args .bld-alpine-324/test/process-linux-test
```

## Working-directory behavior

The wrapper mounts the source tree read-only at its existing absolute path and
mounts only the selected version's build tree from the table above read-write.

Containers run with the invoking user's UID and GID, so persistent build output
remains owned by that user. The stable absolute path also keeps generated
`compile_commands.json` entries usable by host tools.

The wrapper intentionally does not add an init process, preserving container
PID 1 behavior relevant to Linux process tests. Rebuild an image when its
Dockerfile changes, and recreate that environment's build tree after material
compiler or system-library changes.
