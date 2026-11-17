---
title: watchman-wait
category: Invocation
sidebar_position: 3
---

`watchman-wait` waits for changes to files. It uses the watchman service to
efficiently and recursively watch your specified list of paths.

It is suitable for waiting for changes to files from shell scripts. It has some
similarity to `inotifywait` except that it uses the watchman service to watch
files and thus can be used on any of the operating systems supported by
watchman, not just Linux.

It can stop after a configurable number of events are observed. The default is a
single event. You may also remove the limit and allow it to execute
continuously.

`watchman-wait` will print one event per line. The event information includes
your specified list of fields, with each field separated by a space (or your
choice of `--separator`).

Events are consolidated and settled by the watchman server before they are
dispatched to `watchman-wait` so that your script won't start executing until
after the files have stopped changing.

`watchman-wait` is part of [pywatchman](https://pypi.org/project/pywatchman/),
Watchman's Python client, so it needs Python as well as `watchman`. The Windows
release includes it. On other systems, install it with `pip install pywatchman`.

### Paths and Patterns

```bash
$ watchman-wait path [path ...]
```

The primary unit of watching is a path. You must specify a list of one or more
paths that you'd like to wait for. Paths can be files or directories. Each of
the paths in your list must exist at the time that you invoke `watchman-wait` or
an error will be reported and `watchman-wait` will exit.

If you'd like to wait for a file to be created you can watch the directory in
which it will be created. You may further refine your watch by limiting it to a
set of patterns.

```bash
$ watchman-wait . -p '*.so'
```

```bash
$ watchman-wait -p '*.so' -- .
```

Both of the above will wait for a shared object file to be changed in any path
under the current working directory. Since both the `-p` option and the list of
paths accept one or more parameters, the second form shows how to disambiguate
between the list of patterns and the list of paths using the `--` separator.

Patterns are wildmatch style globs that support recursive matching via the `**`
placeholder.

You should always quote your pattern parameters so that they are not evaluated
by your shell.

### Controlling lifetime

There are two primary controls for how long `watchman-wait` will run:

- `-t` or `--timeout` places a time limit on execution
- `-m` or `--max-events` places a limit on the number of events to process

`watchman-wait` will terminate when either the timeout is hit or the max events
limit is hit.

By default there is no time limit, but there is a default limit of a single
event.

You may specify `--max-events 0` to disable the event limit.

### Controlling output

`watchman-wait` will output one line per event. The following options influence
the output:

- `--fields NAME,NAME` - specifies the list of fields to be printed for each
  event. The default is `--fields name` which will print just the `name` of the
  file that was changed. You may use any of the available fields listed in
  [available fields](cmd/query.md#available-fields). The fields will be printed
  in the order you list them.
- `--relative DIR` - the `name` field will be adjusted to be relative to `DIR`
  before it is printed out. The default for `DIR` is the current working
  directory when `watchman-wait` is started.
- `--separator STRING` - if you specified multiple fields, the separator string
  will be used when printing them. The default is `--separator " "` which will
  print the fields with spaces between them.

### Exit Status

The following exit status codes can be used to determine what caused
`watchman-wait` to exit:

- `0` is returned after successfully waiting for event(s)
- `1` in case of a runtime error of some kind
- `2` the `-t`/`--timeout` option was used and that amount of time passed before
  an event was received
- `3` if execution was interrupted (Ctrl-C)
