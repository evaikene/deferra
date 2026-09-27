# Checked JSON examples

These small JSON files are `params` or `result` values, ready to place in the
[JSON-RPC envelope](../transport.md#request-and-response-envelopes). The
request examples are accepted by JobU's production request codecs; the result
examples are accepted by its production result codecs. They use fictitious
names, IDs, and timestamps.

| Example | Demonstrates |
| --- | --- |
| [Queue creation](queue-create.params.json) | Defaulted queue configuration and an idempotency key |
| [Job creation](job-create.params.json) | A symbolic once-now schedule and a CLI payload |
| [Job creation with secret references](job-create-secret.params.json) | Ordered CLI argument references and a named environment reference |
| [Job listing request](job-list.params.json) and its [terminal result](job-list.result.json) | An explicit succeeded-state filter and one finished one-time definition |
| [Unfiltered job listing](job-list-all.params.json) | The raw API's absent-state default, also used by `--request-file` |
| [Run listing](run-list.params.json) | Combined owner/state filters |
| [Maximum attempt number](attempt-get-max.params.json) | The inclusive upper bound for `attempt.get` |
| [Attempt output](attempt-output.params.json) | A retained-byte slice |
| [Secret set](secret-set.params.json) | UTF-8 secret input, never a value-read result |
| [Cron preview](schedule-next.params.json) | Strictly later UTC occurrences |
| [Statistics request](system-stats.params.json) and its [empty result](system-stats.result.json) | One bounded planned-time window grouped by `none`, with honest zero-sample measurements |

The [method pages](../README.md) explain the required fields, defaults,
pagination rules, errors, and safe mutation retries. A JSON file here is a
wire example, not a command to execute against a live daemon.
