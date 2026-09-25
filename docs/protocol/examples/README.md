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
| [Run listing](run-list.params.json) | Combined owner/state filters |
| [Attempt output](attempt-output.params.json) | A retained-byte slice |
| [Secret set](secret-set.params.json) | UTF-8 secret input, never a value-read result |
| [Cron preview](schedule-next.params.json) | Strictly later UTC occurrences |
| [Statistics request](system-stats.params.json) and [empty result](system-stats.result.json) | A bounded planned-time window and honest zero-sample measurements |

The [method pages](../README.md) explain the required fields, defaults,
pagination rules, errors, and safe mutation retries. A JSON file here is a
wire example, not a command to execute against a live daemon.
