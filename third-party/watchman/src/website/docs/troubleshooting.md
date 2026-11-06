---
title: Troubleshooting
---

We try to give directed advice in Watchman error diagnostics, which means that
we will show a link to a section on this page with some context and advice where
we have enough information to do so. Some operating systems provide richer
diagnostic information than others, so we have to resort to more generic advice
in some cases.

The most common cause of problems is hitting system resource limits. There are
finite resources available for filesystem watching, and when they are exceeded
it can impact performance in the best case or prohibit correct operation in the
worst case.

## Ensure that you are on the best available version

It is generally a good idea to make sure that you are using the latest version
of the software, so that you avoid any known issues.

If you are running a pre-built binary provided by your operating system
distribution system, there is a chance that you'll need to build the latest
version from source. You can find instructions for this in
[the installation section](install.md).

## Recrawl

A recrawl is an action that Watchman performs in order to recover from
situations where it believes that it has lost sync with the state of the
filesystem.

The most common cause for a recrawl is on Linux systems where the default
inotify limits are sized quite small. What this means is that the rate at which
your watched roots are generating changes is higher than the kernel can buffer
and relay to the watchman service. When this happens, the kernel detects the
overflow and signals `IN_Q_OVERFLOW`. The recovery is to recursively scan the
root to make sure that we know what is really there and re-sync with the
notification stream.

Frequent recrawls are undesirable because they result in a potentially expensive
full tree crawl, which marks all files as changed and propagates this status to
clients which will in turn perform some action on the (likely falsely) changed
state of the majority of files.

### Avoiding Recrawls

There is no simple formula for setting your system limits; bigger is better but
comes at the cost of kernel memory to maintain the buffers. You and/or your
system administrator should review the workload for your system and the
[System Specific Preparation Documentation](install.md#system-specific-preparation)
and raise your limits accordingly.

### kFSEventStreamEventFlagUserDropped

macOS has a similar internal limit and behavior when that limit is exceeded. If
you're encountering a message like:

```
Recrawled this watch 1 time, most recently because:
/some/path: kFSEventStreamEventFlagUserDropped
```

then you are hitting the limits of your system. There is no direct control over
the limit, but starting in Watchman 3.2 you may increase the
[fsevents_latency](config.md#fsevents-latency) parameter in your
`.watchmanconfig` file.

### I've changed my limits, how can I clear the warning?

The warning will stick until you cancel the watch and reinstate it, or restart
the watchman process. The simplest resolution is to run
`watchman shutdown-server` and re-establish your watch on your next watchman
query.

## Where are the logs?

Watchman places logs in a file named `<STATEDIR>/<USER>.log`, where `STATEDIR`
is set at the time that you built watchman.

If you used the `--enable-statedir=<STATEDIR>` configure option, that will be
the location that holds your logs. If not, the default for `STATEDIR` will be
`<PREFIX>/var/run/watchman`, or for older versions of watchman, the logs may be
placed in `<TMPDIR>/.watchman.<USER>.log`.

_Since 3.8._

Watchman places the logs in a file named `<STATEDIR>/log`, which will typically
be a location like `<PREFIX>/var/run/watchman/<USER>-state/log`. If you're
running a `homebrew` build of watchman, `<PREFIX>` is usually `/usr/local`.

The default log location may be overridden by the `--logfile`
[Server Option](cli-options.md#server-options).

[Quick note on default locations](cli-options.md#quick-note-on-default-locations)
explains what we mean by `<STATEDIR>`, `<TMPDIR>`, `<USER>` and so on.

## <a id="poison-inotify-add-watch"></a>Poison: inotify_add_watch

```
A non-recoverable condition has triggered.  Watchman needs your help!
The triggering condition was at timestamp=1407695600: inotify-add-watch(/my/path) -> Cannot allocate memory
All requests will continue to fail with this message until you resolve
the underlying problem.  You will find more information on fixing this at
https://meta.github.io/watchman/docs/troubleshooting.html#poison-inotify-add-watch
```

If you've encountered this state it means that your _kernel_ was unable to watch
a dir in one or more of the roots you've asked it to watch. This particular
condition is considered non-recoverable by Watchman on the basis that nothing
that the Watchman service can do can guarantee that the root cause is resolved,
and while the system is in this state, Watchman cannot guarantee that it can
respond with the correct results that its clients depend upon. We consider
ourselves poisoned and will fail all requests for all watches (not just the
watch that it triggered on) until the process is restarted.

There are two primary reasons that this can trigger:

- The user limit on the total number of inotify watches was reached or the
  kernel failed to allocate a needed resource
- Insufficient kernel memory was available

The resolution for the former is to revisit
[System Specific Preparation Documentation](install.md#system-specific-preparation)
and raise your limits accordingly.

The latter condition implies that your workload is exceeding the available RAM
on the machine. It is difficult to give specific advice to resolve this
condition here; you may be able to tune down other system limits to free up some
resources, or you may just need to install more RAM in the system.

### I've changed my limits, how can I clear the error?

The error will stick until you restart the watchman process. The simplest
resolution is:

_Since 4.6_

```bash
$ watchman watch-del-all
$ watchman shutdown-server
```

_Before 4.6_

```bash
$ rm <STATEDIR>/state       # see above for what STATEDIR means
$ watchman --no-spawn --no-local shutdown-server
```

If you have not actually resolved the root cause you may continue to trigger and
experience this state each time the system trips over these limits.

## Poison: opendir

```
A non-recoverable condition has triggered.  Watchman needs your help!
The triggering condition was at timestamp=1407695600: opendir(/my/path) -> Too many open files in system
All requests will continue to fail with this message until you resolve
the underlying problem.  You will find more information on fixing this at
https://meta.github.io/watchman/docs/troubleshooting.html#poison-opendir
```

If you've encountered this state it means that your entire system had too many
open files, and that this prevented watchman from tracking the changes on your
system. In this case, the error isn't related to filesystem watching but to
other (likely) misbehaving processes on your system; it's usually indicative of
a runaway program or set of programs consuming resources, but in some cases it
may just be that your system workload requires that you increase your system
limits for the number of files.

### How do I resolve this?

[Follow these directions](troubleshooting.md#i-39-ve-changed-my-limits-how-can-i-clear-the-error)

If the issue persists, consult your system administrator to identify what is
consuming these resources and remediate it, or to increase your system limits.

## FSEvents

FSEvents is the file watching facility on macOS. There are few diagnostics that
can help diagnose issues with FSEvents; the API itself gives little feedback on
a number of error cases and instead emits rather cryptic error messages to the
log file.

If you got here because an error message told you to read this section, it will
have also asked you to look at your log file. If you are using an older version
of watchman and encounter the error message `FSEventStreamStart failed`, then
you should locate your log file (see [Where are the logs?](#where-are-the-logs)
above) and look for lines that mention FSEvents and then consult the information
below.

### FSEventStreamStart: register_with_server: ERROR: f2d_register_rpc() => (null) (-21)

Nobody outside of Apple is sure what precisely this means, but it indicates that
the fsevents service has gotten in a bad state. Possible reasons for this may
include:

- There are too many event stream clients
- One or more event stream clients has gotten in a bad state and is somehow
  impacting the fsevents service

To resolve this issue, you may wish to try the following, which are
progressively more invasive:

- Avoid establishing multiple overlapping watches within the same filesystem
  tree, especially for large trees. We recommend watching only the root of a
  project or repo and not watching sub-trees within that tree. Organizations
  with large trees may wish to deploy the
  [root_restrict_files](config.md#root-restrict-files) configuration option so
  that watchman will only allow watching project roots.
- Close or restart other applications that are using fsevents. Some examples
  are:
- editors such as Sublime Text and TextMate.
- Many nodejs packages and Grunt style workflows make use of fsevents. Make sure
  that you upgrade nodejs to at least version `v0.11.14`. If possible, configure
  your nodejs packages to use either [sane](https://www.npmjs.com/package/sane)
  or [fb-watchman](https://www.npmjs.com/package/fb-watchman) for file watching
  as this will consolidate the number of fsevents watches down to just the set
  maintained by watchman.
- Restart the fsevents service: `sudo pkill -9 -x fseventsd`
- Restart your computer

## Triggers/Subscriptions don't fire on macOS

There is a rare fsevents bug that can prevent any notifications from working in
directories where the case of the name of a directory in the kernel has an
inconsistency.

You can test whether this is happening to you by following
[the instructions for the find-fsevents-bugs tool](https://github.com/andreyvit/find-fsevents-bugs).

If it is happening to you, the resolution is to rename the directories
highlighted by the tool.

You can read more about this issue in the following resources:

- [Knowledge base article for LiveReload](http://feedback.livereload.com/knowledgebase/articles/86239-os-x-fsevents-bug-may-prevent-monitoring-of-certai)
- [issue for the Ruby fsevents module](https://github.com/thibaudgg/rb-fsevent/issues/10)
- [Open Radar bug report](http://openradar.appspot.com/10207999)

## ReactNative: Watcher took too long to load

There was an issue that was the result of umask affecting the permissions of the
launchd plist file that Watchman uses to set up your watchman service on OS X.
This issue was resolved in Watchman version 3.1.

To update:

```bash
$ watchman shutdown-server
$ brew update
$ brew reinstall watchman
```
