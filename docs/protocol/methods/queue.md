# Queue retained statistics

`queue.stats` has the [system.stats](system.md) query fields and result shape,
but an initial request requires exactly one existing queue selector:
`queue_id` or `queue_name`. Here `queue_id` selects the scope; it is not an
additional optional filter. The daemon resolves the selector to a stable queue
ID before reading statistics and stores that ID in any continuation cursor.
An ID can select a soft-deleted queue; a historical name must resolve
unambiguously under the existing queue lookup rules.

```json
{"queue_name":"reports","group_by":"state","limit":20}
```

Resume with only `{"cursor":"<opaque token>"}`: do not repeat the selector,
filter, window, group, or limit. A system-statistics cursor cannot be used for
queue statistics and vice versa. An unknown selector uses the existing queue
not-found error. A historical name matching multiple deleted queues returns
application `-32000` with `category: Conflict` and
`code: jobu.queue.ambiguous_deleted_name`; select the queue by ID. A missing
selector, both selectors, an extra field, or a selector beside `cursor` returns
JSON-RPC invalid params (`-32602`). Other service errors use application
`-32000`; fatal reads close admission as described on the system page.
The CLI route is `queue stats`; the typed client member is
`ControlClient::queue_statistics()`.
