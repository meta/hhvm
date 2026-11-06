---
title: Source Control Aware Queries
category: Queries
---

_Since 2021.08.30_

[Git support](https://github.com/meta/watchman/pull/934) has been added 🎊

_Since 4.9._

_The [capability](capabilities.md) name associated with this enhanced
functionality is `scm-since`._

The capability name for this is `scm-hg`. The internal architecture allows
supporting other source control systems quite easily; it just needs someone to
implement and test them!

A common pattern for tools that consume watchman is wanting to reason about the
changes in a version controlled repository. For most repos it is fine to simply
receive information about all changed files as they are updated, even during a
rebase over several days of work by others.

For very large or very busy repositories, where a great many files can change
over a short period of time, it can be desirable to get a minimized set of
information about the changes.

For example, if your tool has the ability to load some pre-built data from some
artifact storage, rather than processing many hundreds of changed files
incrementally you may want to take the merge base of local changes and use that
to locate the pre-built data and process only the delta between that state and
the current state of the repo.

An illustration may help. Here we see that a user has a stack of two commits
based off the symbolic `main` commit. In this scenario, `main` is tracking the
tip of the repo to which the local repo is published, and the user is checked
out at the 6b38a5 commit:

```
| @  6b38a5  wez
| |  Add cats.cpp
| |
| o  fa2e92  wez
|/   Add cat.jpg
|
o f12345 main
```

Now the user synchronizes their repo with the remote, fetching the commits but
not changing their work yet. This is often combined with the step that follows,
but we are breaking it out here for the purposes of illustration. This is
equivalent to running `hg pull` or `git fetch`:

```
o  fabf87  coworker     main
.  Amazing new feature
.
| @  6b38a5  wez
| |  Add cats.cpp
| |
| o  fa2e92  wez
|/   Add cat.jpg
|
o
```

The ellipsis portion of the DAG represents uninteresting commits to `wez`; there
may be hundreds of files changed by those commits, but `wez` only cares about
the work in their local branch of the DAG.

Now `wez` wants to rebase their work on main. This would be done using a command
like `hg rebase -d main -s fa2e92`:

```
| @  bbbbbb  wez
| |  Add cats.cpp
| |
| o  aaaaaa  wez
|/   Add cat.jpg
|
o  fabf87  coworker     main
.  Amazing new feature
.
```

The crucial part of this is what happens to the working copy; assuming that we
now land on commit `bbbbbb`, Watchman will observe changes for all of the
hundreds of files that changed across the rebase and pass this information on to
the tools that are subscribed or are querying for this information.

If your tooling is source control aware then you can ask watchman to run since
queries in a mode where it will return you information about the merge base with
`main` and the minimized set of files that changed.

To enable this mode you issue a query using a new _fat clock_ as the `since`
parameter for the query:

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "since": {
      "scm": {
        "mergebase-with": "main"
      }
  },
  "expression": ["type", "f"],
  "fields": ["name"]
}]
EOT
```

This particular `since` value starts with an unspecified clock value and
requests that watchman run the query in source control aware mode, using the
symbolic name `main` to compute the merge base for the commit graph.

If we look back to the illustrations above and rewind to the first scenario, the
results of this query will look something like this:

```
{
   "clock": {
       "clock": "c:123:123",
       "scm": {
            "mergebase": "f12345",
            "mergebase-with": "main"
       }
    },
    "files": ["cat.jpg", "cats.cpp"]
}
```

This result informs the client of the merge base with main (which happens to be
main itself) and the list of changes since that merge base.

To get the next incremental change the client feeds that clock value back in to
its next query. Looking back to the second illustration above, if we were to run
this query after the running `hg pull` (note that this doesn't change the
working copy):

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "since": {
       "clock": "c:123:123",
       "scm": {
            "mergebase": "f12345",
            "mergebase-with": "main"
       }
  },
  "expression": ["type", "f"],
  "fields": ["name"]
}]
EOT
```

we'd get this result:

```json
{
   "clock": {
       "clock": "c:123:124",
       "scm": {
            "mergebase": "f12345",
            "mergebase-with": "main"
       }
    },
    "files": []
}
```

Note that the `files` list is empty because we didn't change any files, and note
that one of the numeric portions of the clock string has changed.

Also note that the mergebase revision remains the same because we also didn't
rebase the commit yet.

This is a little white lie: the reality is that some files did change in the
version control system, and with the expression we're using we would see them,
but they are not part of the working copy so we're omitting them for the clarity
of this example.

Now if we rebase and update to the rebased revision (taking us to the last of
the illustrations from above), we'd run this query, feeding in the clock from
the last query to get the correct incremental result:

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "since": {
       "clock": "c:123:124",
       "scm": {
            "mergebase": "f12345",
            "mergebase-with": "main"
       }
  },
  "expression": ["type", "f"],
  "fields": ["name"]
}]
EOT
```

we'd get this result:

```json
{
   "clock": {
       "clock": "c:123:125",
       "scm": {
            "mergebase": "fabf87",
            "mergebase-with": "main"
       }
    },
    "files": ["cat.jpg", "cats.cpp"]
}
```

Note that the mergebase reported in the clock has changed and note that the list
of files reported is just the two from our commit stack despite there being
hundreds of files that were physically updated on the disk.

Your client can now lookup some state based on the `fabf87` revision and
download it, and can then incrementally apply the computation for `cat.jpg` and
`cats.cpp` on top of that state.

If your client doesn't know how to do this, then you shouldn't use this source
control aware query mode!

## Source Control Aware Subscriptions

You can also use the same source control awareness in your subscriptions. This
is basically the same procedure as making queries above, but there are some
preconditions and things to note:

- Watchman needs the cooperation of the source control system to know when it
  should defer events.
- Source control aware subscriptions implicitly enable `defer_vcs` and
  `defer:["hg.update"]`. As with the point above, this is to ensure that you
  don't get notified about files changing during the working copy update
  operation; that would defeat the point of using source control awareness.

To initiate a source control aware subscription:

```json
["subscribe", "/path/to/root", "mysubscriptionname", {
  "fields": ["name"],
  "since": {
    "scm": {
      "mergebase-with": "main"
    }
  }
}]
```

You'll then receive subscription responses as files change; those responses will
contain _fat clock_ values for the `since` and `clock` fields:

```json
{
  "subscription": "mysubscriptionname",
  "clock": {
    "clock": "c:1234:125",
    "scm": {
      "mergebase": "fabf87",
      "mergebase-with": "main",
    }
  },
  "since": {
    "clock": "c:1234:123",
    "scm": {
      "mergebase": "f12345",
      "mergebase-with": "main",
    }
  },
  "files": ["cat.jpg", "cats.cpp"],
  "root":  "/path/to/root"
}
```

The `clock` field holds the value of the clock and the merge base as of the
subscription notification.

The `since` field holds the _fat clock_ that was returned in the `clock` field
from the prior subscription update. It is present as a convenience for you; you
can compare the `mergebase` fields between the two to determine that the merge
base changed in this update. This is an important detail because more files in
the working copy have been physically changed than are reflected in the `files`
list; your tooling will need to so something appropriate to ensure that it
computes a consistent and correct result.

### `state-enter` & `state-leave`

Source control aware subscriptions will always include a _fat clock_ in their
responses, however, only the regular clock is provided in `state-enter` and
`state-leave` notifications. This is because computing the source control
information is a non-trivial operation and could increase latency.
