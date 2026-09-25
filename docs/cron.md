# Cron schedules in JobU

Use a cron schedule when a job should recur at particular **local calendar
times**. The expression chooses minutes, hours, and dates; the timezone tells
JobU how to turn each matching local time into a UTC instant. For a one-time
job, use `--at` or `--now` instead. A cron schedule works for either a
command-line or an HTTP job.

## The five fields

Write exactly five fields in this order, separated by spaces or tabs:

```text
minute hour day-of-month month day-of-week
```

| Field | Values | Example |
| --- | --- | --- |
| Minute | `0`–`59` | `30` means half past the hour |
| Hour | `0`–`23` | `9` means 09:00 local time |
| Day of month | `1`–`31` | `1` means the first day |
| Month | `1`–`12` or `JAN`–`DEC` | `JAN` means January |
| Day of week | `0`–`7` or `SUN`–`SAT` | `0`, `7`, and `SUN` mean Sunday |

Month and weekday names ignore letter case. An expression has no seconds or
year field. JobU considers whole-minute times only. An expression can contain
at most 512 bytes; line breaks are not field separators.

Within a field, `*` matches every value, `a,b` makes a list, `a-b` includes
both endpoints, and `/n` steps through a wildcard or range from its first
value. For example, `*/15` in the minute field selects `0,15,30,45`, while
`9-17/2` in the hour field selects `9,11,13,15,17`. You can combine members
with commas, such as `0,30` or `JAN,MAR-MAY`. A step cannot follow a single
value: use `5`, not `5/2`.

Weekdays have two special rules:

- A weekday range may wrap through Sunday. `FRI-MON` selects Friday, Saturday,
  Sunday, and Monday. `FRI-MON/2` selects Friday and Sunday; the step starts at
  the written first day.
- A weekday wildcard step such as `*/2` is invalid because it has no explicit
  starting day. Write `SUN-SAT/2` for Sunday, Tuesday, Thursday, and Saturday.
  Numeric `0` and `7` both mean Sunday, but one weekday field cannot contain
  both forms.

Other fields do not wrap: `DEC-JAN` and `17-9` are invalid ranges. JobU
requires **both** day-of-month and day-of-week to match. For example,
`0 9 13 * FRI` means 09:00 only when the 13th is a Friday; it does not mean
every Friday plus every 13th.

## Shorthand aliases

These are the complete aliases JobU accepts. Each expands to the expression
shown and uses the schedule's timezone.

| Alias | Exact expression | Meaning |
| --- | --- | --- |
| `@hourly` | `0 * * * *` | At minute 0 of every local hour |
| `@daily` | `0 0 * * *` | At local midnight every day |
| `@weekly` | `0 0 * * SUN` | At local midnight every Sunday |

Alias spelling is lowercase and exact: `@DAILY` is invalid. Other aliases,
including `@monthly` and `@yearly`, are not accepted. To run on the first day
of each month, use `0 0 1 * *`.

## Timezones and daylight saving time

Use `UTC` or an IANA timezone name available on the daemon, such as
`Europe/Tallinn`. `jobuctl` defaults to `UTC` when `--timezone` is omitted;
the JSON-RPC cron schedule object requires an explicit `timezone`. A schedule
at `09:00` in a named timezone follows local 09:00 as its UTC offset changes.
Returned occurrence timestamps are always UTC and end in `Z`.

When clocks jump forward and a scheduled local time does not exist, JobU
shifts that occurrence forward by the size of the clock change. When clocks
move back and a local time occurs twice, JobU uses the **earlier UTC**
instant once. For example, in `Europe/Tallinn` in 2024:

| Expression | Local date and rule | Chosen UTC occurrence |
| --- | --- | --- |
| `30 3 31 MAR *` | March 31 at 03:30 does not exist; it shifts to 04:30 | `2024-03-31T01:30:00Z` |
| `30 3 27 OCT *` | October 27 at 03:30 occurs twice; the first is used | `2024-10-27T00:30:00Z` |

## Check a schedule before creating a job

Ask the daemon to validate the expression and preview its next occurrences:

```sh
jobuctl --socket /run/jobu.sock schedule validate '0 9 * * MON-FRI' \
    --timezone Europe/Tallinn
jobuctl --socket /run/jobu.sock schedule next '@daily' \
    --timezone UTC --after 2030-01-01T00:00:00Z --count 3 --json
```

Replace the socket path and `--after` time with your own. `--after` is a UTC
instant ending in `Z`; the preview returns occurrences **strictly later**
than it. The default preview count is 5, and the accepted range is 1–200.
The preview above returns January 2, 3, and 4 at 00:00 UTC. Validation checks
the expression and timezone; preview also checks that matching occurrences
can actually be found. Neither command creates or changes a job.

After checking the schedule, create a queue and a recurring job:

```sh
jobuctl --socket /run/jobu.sock queue create reports
jobuctl --socket /run/jobu.sock job create \
    --queue-name reports --type cli --cron '@daily' \
    --timezone Europe/Tallinn --command /bin/echo --arg=ready
```

The equivalent protocol schedule value is
`{"kind":"cron","expression":"@daily","timezone":"Europe/Tallinn"}`.
See the [jobuctl guide](jobuctl.md) for job settings and the
[schedule methods](protocol/methods/schedule.md) for JSON-RPC request and
result fields.

## What happens when execution is late

A preview shows planned calendar occurrences, not a promise of exact start
time. Queue capacity, a suspended resource, or an earlier attempt can delay
execution. Retries are further attempts of the **same run**. If the job still
has a cron schedule and has not been deleted, JobU creates its next recurring
run after the current scheduled run becomes terminal. That next run is
strictly after the later of the current run's planned time and completion
time. JobU does not create one run for every calendar occurrence missed while
work was delayed.

An expression can be syntactically valid but have no possible date, such as
`0 0 31 FEB *`; preview then reports `jobu.schedule.no_future_occurrence`.
Malformed expressions report `jobu.schedule.invalid_expression`, and a
missing or invalid daemon timezone reports `jobu.schedule.invalid_timezone`.
