# jobuctl

`jobuctl` manages JobU queues and job definitions through a running `jobud` daemon's local socket.

## Help and version

Help is available without a daemon or socket path:

```sh
jobuctl
jobuctl --help
jobuctl queue
jobuctl queue --help
jobuctl queue create --help
jobuctl queue add --help
jobuctl help queue add
jobuctl job create -h
jobuctl --version
```

With no arguments, `jobuctl` lists the command groups. A group without an action lists its commands. Command help lists operands, options, defaults, restrictions, and an example. `-h` and `--help` work even when required command operands are missing. Help and version do not contact the daemon or read files or standard input.

An unknown group, command, or option is an error, even alongside `--help`. An option that requires a value must still have one: `queue get --name --help` reports a missing value. Successful help/version exits with status 0; command syntax errors exit with status 2 and print an error and relevant usage to standard error.

## Available commands

| Group | Commands |
| --- | --- |
| `system` | `info` |
| `queue` | `create` (`add`), `get`, `list`, `update`, `suspend`, `resume`, `delete` |
| `job` | `create` (`add`), `get`, `list`, `update`, `suspend`, `resume`, `move`, `delete` |

`queue add` and `job add` are aliases for `queue create` and `job create`. Each alias accepts the same options and performs the same operation as its canonical command.

## Connecting to the daemon

Remote commands require `--socket PATH`. This option can precede or follow the command path; supply it only once:

```sh
jobuctl --socket /run/jobu.sock system info
jobuctl queue list --socket /run/jobu.sock
jobuctl queue --socket /run/jobu.sock create reports
```

Choose the path used by your daemon. There is no automatic socket discovery.

Queue operations select a queue with exactly one of `--id UUID` or `--name NAME`. Job creation and moving use exactly one of `--queue-id UUID` or `--queue-name NAME`. Job update, move, and delete require `--revision N`; use the current revision shown by `job get`.

For example:

```sh
jobuctl --socket /run/jobu.sock queue create reports
jobuctl --socket /run/jobu.sock job create \
    --queue-name reports --type cli --at 2030-01-01T00:00:00Z \
    --command /bin/echo --arg=hello
```

Use a future UTC timestamp appropriate for your job. Run `jobuctl job create --help` for the supported CLI and HTTP creation options.

## Literal arguments

Repeat `--arg` to pass multiple arguments to a CLI job. Each value becomes one argument, preserving order and empty strings. Use `--arg=VALUE` for values beginning with a dash, especially values that resemble `jobuctl` options:

```sh
jobuctl --socket /run/jobu.sock job create \
    --queue-name reports --type cli --at 2030-01-01T00:00:00Z \
    --command /bin/echo --arg=--help --arg=-h --arg= --arg='two words'
```

Here `--help` and `-h` are arguments to the scheduled program. They do not request `jobuctl` help. Attached values such as `--name=--help` are also literal values.

`--` ends option handling. Following tokens are positional data where the command accepts them; otherwise they are extra-operand errors. For example, this creates a queue named `--help`:

```sh
jobuctl --socket /run/jobu.sock queue create -- --help
```
