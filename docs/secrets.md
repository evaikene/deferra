# Named secrets

JobU stores named byte values separately from job payloads. A job refers to a name;
the scheduler resolves its current value immediately before each execution attempt.
Job definitions, run snapshots, and idempotency records keep the original references.

The C++ `SecretService` and the public JSON-RPC methods `secret.set`,
`secret.list`, and `secret.delete` support setting, listing metadata, and
deleting secrets. See [Secret administration methods](protocol/methods/secret.md)
for wire requests and results, and the [jobuctl guide](jobuctl.md#named-secrets)
for file/stdin commands. The examples below describe payload templates.

## Values and references

A value can contain 0–65,536 raw bytes, including an empty value. Names are at most
128 bytes and consist of dot-separated segments. Each segment begins with a
lowercase ASCII letter; remaining characters are lowercase letters, digits, or
underscores. For example, `reports.token` is valid. Names are not normalized.

A reference replaces one whole value:

```json
{"secret":"reports.token"}
```

There is no interpolation, concatenation, default value, or transformation.
For an authorization header, store the complete value, including any `Bearer `
prefix, in the secret.

| Payload position | Reference behavior |
| --- | --- |
| CLI `arguments[i]` | Supplies one complete argument |
| CLI `environment[name]` | Supplies an environment value; `PATH` and reserved `JOBU_*` names cannot use references |
| HTTP `headers[i].value` | Supplies a complete header value and always marks that header sensitive |
| HTTP `body` | Supplies the raw request-body bytes |

For example, a CLI payload can use an environment reference:

```json
{
  "command":"/usr/local/bin/report",
  "arguments":["--daily"],
  "environment":{"REPORT_TOKEN":{"secret":"reports.token"}}
}
```

An HTTP payload can refer to both a header and a binary body:

```json
{
  "url":"https://api.example.test/events",
  "method":"POST",
  "headers":[{"name":"Authorization","value":{"secret":"events.authorization"}}],
  "body":{"secret":"events.body"}
}
```

Executable paths, working directories, URLs, methods, header names, environment
names, and schedules remain literal. An object containing `secret` in an unrelated
additive payload field is not resolved. Recognized reference objects must contain
only the `secret` member. A template may contain at most 256 reference occurrences.

CLI argument and environment values must be valid UTF-8 without NUL. HTTP header
values must satisfy normal header validation, including no CR, LF, or NUL. HTTP
body values remain binary and do not require valid UTF-8. Original and expanded
serialized payloads are each limited to 256 KiB; runner-specific request limits
also apply after expansion and injected execution metadata.

## Execution, rotation, and deletion

Create and update require referenced names to exist. Setting an existing secret
replaces its bytes atomically while preserving its creation timestamp. A set or
list result contains only the name and creation/update timestamps, with no value,
size, digest, or preview. There is no administrative secret-value read operation.

Each attempt resolves afresh, including retries in either blocking or rescheduled
mode. Repeated uses of the same name share one lookup within an attempt. Rotation
after an attempt has been prepared affects subsequent attempts, not the already
prepared request. It does not change the stored job/run template or the identity
of an idempotent creation request.

Missing names, invalid destination bytes, and excessive expansion produce a safe
terminal failed attempt without launching a process or sending an HTTP request.
Storage or unexpected provider failures follow the scheduler's fatal failure
path. External execution starts only after the durable Running transition commits.
Recovery validates stored templates without resolving them.

Deletion is blocked while either a current job definition or any nonterminal run
snapshot references the name. Updating a definition does not release references
held by an older running or retrying snapshot. Terminal historical snapshots alone
do not prevent deletion. There is no force-delete option.

## Redirects and generated diagnostics

Secret-derived headers are sensitive even when the payload explicitly sets
`sensitive` to `false` or uses an ordinary custom header name. When redirects are
enabled, the HTTP client retains them for same-origin redirects and removes them
before crossing an origin boundary. Removed headers stay absent on later hops,
including a return to the original origin. This protection concerns headers;
request bodies follow the normal HTTP redirect method/body policy.

Generated errors, attempt results, and logs use safe codes and metadata rather
than resolved arguments, environment values, headers, or bodies. Administrative
job reads return original references. These guarantees do not imply filtering of
application-generated output.

## Storage, process exposure, and captured output

Secret values are plaintext BLOBs in SQLite. Access to database files and their
copies is privileged access to these values. JobU does not promise encrypted
storage or secure erasure of allocator, library, or database copies.

Resolved arguments are passed to the operating system and may be visible through
process inspection. Prefer environment references when argument exposure matters;
environments are still accessible to the target program and potentially to
privileged process inspection. Avoid putting secret literals in command lines,
job definitions, or idempotency requests.

For `jobuctl secret set`, use `--file PATH` or `--stdin` to pass raw bytes
without putting the value in the process argument vector. Input is limited to
65,536 bytes and keeps a trailing newline. `--request-file` can carry a
structured `utf8` or base64 value; protect that file like the raw input file.
CLI metadata and generated errors do not print the value, length, or preview.

A program can echo an argument or environment value to stdout/stderr. An HTTP
server can echo request secrets in response bodies or headers. JobU captures those
bytes faithfully; it does not apply global secret-byte substitution. Retained
output may therefore contain secrets, and should receive corresponding access
protection.

The existing `output.capture` attribute controls retention:

| Value | Behavior |
| --- | --- |
| `none` | Do not retain subprocess output or HTTP response output |
| `on_error` (default) | Retain output for unsuccessful attempts |
| `always` | Retain output for both successful and unsuccessful attempts |

CLI limits are `output.stdout_limit` and `output.stderr_limit`, each defaulting to
1 MiB. HTTP limits are `output.http_body_limit` (1 MiB) and
`output.http_headers_limit` (64 KiB). Capture limits bound retained bytes; they
are not redaction controls. `on_error` can still retain echoed secrets from a
failed attempt, and disabling JobU capture does not control logs or other storage
owned by the executed program or remote server.
