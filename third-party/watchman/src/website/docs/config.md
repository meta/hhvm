---
title: Configuration Files
category: Invocation
sidebar_position: 7
---

Watchman looks for configuration files in two places:

- The global configuration file `/etc/watchman.json`
- The root specific configuration file `.watchmanconfig`

When watching a root, if a valid JSON file named `.watchmanconfig` is present in
the root directory, watchman will load it and use it as a source of
configuration information specific to that root.

The global configuration path can be changed by passing the `--enable-conffile`
option to configure when you build watchman. This documentation refers to it as
`/etc/watchman.json` throughout, just be aware that your particular installation
may locate it elsewhere. In addition, the environmental variable
`$WATCHMAN_CONFIG_FILE` will override the default location.

If the global configuration file does not exist, Watchman will fall back on that
path with ".default" appended (e.g. /etc/watchman.json.default). This allows the
Watchman system package to provide different configuration defaults, like
setting enforce_root_files to true.

Changes to the `.watchmanconfig` or `/etc/watchman.json` files are not picked up
automatically; you will need to remove and re-add the watch (for
`.watchmanconfig`) or restart watchman (for `/etc/watchman.json`) for those
changes to take effect.

### Resolution / Scoping

There are three configuration scopes:

- **local** - the option value is read from the `.watchmanconfig` file in the
  associated root.
- **global** - the option value is read from the `/etc/watchman.json` file
- **fallback** - the option value is read from the `.watchmanconfig` file. If
  the option was not present in the `.watchmanconfig` file, then read it from
  the `/etc/watchman.json` file.

This table shows the scoping and availability of the various options:

| Option                      | Scope    | Since version     |
| --------------------------- | -------- | ----------------- |
| `settle`                    | local    |
| `root_restrict_files`       | global   | deprecated in 3.1 |
| `root_files`                | global   | 3.1               |
| `enforce_root_files`        | global   | 3.1               |
| `illegal_fstypes`           | global   | 2.9.8             |
| `illegal_fstypes_advice`    | global   | 2.9.8             |
| `ignore_vcs`                | local    | 2.9.3             |
| `ignore_dirs`               | local    | 2.9.3             |
| `gc_age_seconds`            | local    | 2.9.4             |
| `gc_interval_seconds`       | local    | 2.9.4             |
| `fsevents_latency`          | fallback | 3.2               |
| `idle_reap_age_seconds`     | local    | 3.7               |
| `hint_num_files_per_dir`    | fallback | 3.9               |
| `hint_num_dirs`             | fallback | 4.6               |
| `suppress_recrawl_warnings` | fallback | 4.7               |

### Configuration Options

### settle

Specifies the settle period in _milliseconds_. This controls how long the
filesystem should be idle before dispatching triggers. The default value is 20
milliseconds.

### root_files

_Since 3.1._

Specifies a list of files that, if present in a directory, identify that
directory as the root of a project.

If left unspecified, to aid in transitioning between versions, watchman will use
the value of the now deprecated [root_restrict_files](#root_restrict_files)
configuration setting.

If neither `root_files` nor `root_restrict_files` is specified in the
configuration, watchman will use a default value consisting of:

- `.git`
- `.hg`
- `.svn`
- `.watchmanconfig`

Watchman will add `.watchmanconfig` to whatever value is specified for this
configuration value if it is not present.

This example causes only `.watchmanconfig` to be considered as a project root
file:

```json
{
  "root_files": [".watchmanconfig"]
}
```

See the [watch-project](cmd/watch-project.md) command for more information.

### enforce_root_files

_Since 3.1._

This is a boolean option that defaults to `false`. If it is set to `true` then
the [watch](cmd/watch.md) command will only succeed if the requested directory
contains one of the files listed by the [root_files](#root_files) configuration
option, and the [watch-project](cmd/watch-project.md) command will only succeed
if a valid project root is found.

If left unspecified, to aid in transitioning between versions, watchman will
check to see if the now deprecated [root_restrict_files](#root_restrict_files)
configuration setting is present. If it is found then the effective value of
`enforce_root_files` is set to `true`.

### root_restrict_files

_Deprecated starting in version 3.1; use [root_files](#root_files) and
[enforce_root_files](#enforce_root_files) to effect the same behavior._

Specifies a list of files, at least one of which should be present in a
directory for watchman to add it as a root. By default there are no
restrictions.

For example,

```json
{
  "root_restrict_files": [".git", ".hg"]
}
```

will allow watches only in the top level of Git or Mercurial repositories.

### illegal_fstypes

Specifies a list of filesystem types that watchman is prohibited to attempt to
watch. Watchman will determine the filesystem type of the root of a watch; if
the typename is present in the `illegal_fstypes` list, the watch will be
prohibited. You may also specify `illegal_fstypes_advice` as a string with
additional advice to your user. The purpose of this configuration option is
largely to prevent the use of Watchman on network mounted filesystems. On Linux
systems, Watchman may not be able to determine the precise type name of a
mounted filesystem. If the filesystem type is not known to watchman, it will be
reported as `unknown`.

For example,

```json
{
  "illegal_fstypes": ["nfs", "cifs", "smb"],
  "illegal_fstypes_advice": "use a local directory"
}
```

will prevent watching dirs mounted on network filesystems and provide the advice
to use a local directory. You may omit the `illegal_fstypes_advice` setting to
use a default suggestion to relocate the directory to local disk.

### ignore_vcs

Apply special VCS ignore logic to the set of named dirs. This option has a
default value of `[".git", ".hg", ".svn", ".jj"]`. Dirs that match this option
are observed and watched using special shallow logic. The shallow watch allows
watchman to mildly abuse the version control directories to store its query
cookie files and to observe VCS locking activity without having to watch the
entire set of VCS data for large trees.

### ignore_dirs

Dirs that match are completely ignored by watchman. This is useful to ignore a
directory that contains only build products and where file change notifications
are unwanted because of the sheer volume of files.

For example,

```json
{
  "ignore_dirs": ["build"]
}
```

would ignore the `build` directory at the top level of the watched tree, and
everything below it. It will never appear in the watchman query results for the
tree.

On Linux systems, `ignore_dirs` is respected at the OS level; the kernel simply
will not tell watchman about changes to ignored dirs. macOS and Windows have
limited or no support for this, so watchman needs to process and ignore this
class of change.

For large trees or especially busy build dirs, it is recommended that you move
the busy build dirs out of the tree for more optimal performance.

Since version 2.9.9, if you list a dir in `ignore_dirs` that is also listed in
`ignore_vcs`, the `ignore_dirs` placement will take precedence. This may not
sound like a big deal, but since `ignore_vcs` is used as a hint to for the
placement of [cookie files](cookies.md), having these two options overlap in
earlier versions would break watchman queries.

_Since 4.6._

On macOS the first 8 items listed in `ignore_dirs` can be accelerated at the OS
level. This means that changes to those paths are not even communicated to the
watchman service. Entries beyond the first 8 are processed and ignored by
watchman. If your workload is prone to recrawl events you will want to
prioritize your `ignore_dirs` list so that the most busy ignored locations
occupy the first 8 positions in this list.

### gc_age_seconds

Deleted files (and dirs) older than this are periodically pruned from the
internal view of the filesystem. Until they are pruned, they will be visible to
queries but will have their `exists` field set to `false`. Once they are pruned,
watchman will remember the most recent clock value of the pruned nodes. Any
since queries based on a clock prior to the last prune clock will be treated as
a fresh instance query. This allows a client to detect and choose how to handle
the case where they have missed changes. See `is_fresh_instance` elsewhere in
this document for more information. The default for this is `43200` (12 hours).

### gc_interval_seconds

How often to check for, and prune out, deleted nodes per the `gc_age_seconds`
option description above. The default for this is `86400` (24 hours). Set this
to `0` to disable the periodic pruning operation.

### fsevents_latency

Controls the latency parameter that is passed to `FSEventStreamCreate` on macOS.
The value is measured in seconds. The fixed value of this parameter prior to
version 3.2 of watchman was `0.0001` seconds. Starting in version 3.2 of
watchman, the default is now `0.01` seconds and can be controlled on a per-root
basis.

If you observe problems with `kFSEventStreamEventFlagUserDropped` increasing the
latency parameter will allow the system to batch more change notifications
together and operate more efficiently.

### fsevents_try_resync

This is macOS specific.

_Since 4.6._

Defaults to `false`. If set to `true`, if a watch receives a
`kFSEventStreamEventFlagUserDropped` event, attempt to resync from the
`fsevents` journal if it is available. The journal may not be available if one
or more volumes are mounted read-only, if the administrator has purged the
journal, or if the `fsevents` id numbers have rolled over.

This resync operation is advantageous because it effectively allows rewinding
and replaying the event stream from a known point in time and avoids the need to
recrawl the entire watch.

If this option is set to `false`, or if the journal is not available, the
original strategy of recrawling the watched directory tree is used instead.

_Since 4.7._

The default changed to `true`. In addition, this resync strategy is now also
applied to `kFSEventStreamEventFlagKernelDropped` events.

_Since December 2021._

The default changed to `false`. There are possible undiagnosed correctness
issues with this setting.

### prefer_split_fsevents_watcher

This is macOS specific.

Defaults to `false`. If set to `true`, Watchman will use several FSEvents
streams to watch a directory hierarchy instead of a single stream. This has been
shown to significantly reduce the number of `kFSEventStreamEventFlagUserDropped`
events for workflows issuing heavy writes to a top-level directory that is
listed in [ignore_dirs](#ignore_dirs).

### idle_reap_age_seconds

_Since 3.7._

How many seconds a watch can remain idle before becoming a candidate for
reaping, measured in seconds. The default for this is `432000` (5 days). Set
this to `0` to prevent reaping.

A watch is considered to be idle when it has had no commands that operate on it
for `idle_reap_age_seconds`. If an idle watch has no triggers and no
subscriptions then it will be cancelled, releasing the associated operating
system resources, and removed from the state file.

### hint_num_files_per_dir

_Since 3.9._

Used to pre-size hash tables used to track files per directory. This is most
impactful during the initial crawl of the filesystem. Setting this too small
will increase the chance of a hash insert having a collision and drive up the
cost of the insert and subsequent gets.

Prior to version 3.9 of watchman this value was fixed at `2`. Starting in
version 3.9 the default value is `64` and can be configured via this setting in
the `.watchmanconfig` or the global `/etc/watchman.json` configuration file.

Setting this value very large increases the memory overhead per directory in the
tree; the value is rounded up to the next power of two and pre-allocated in an
array of pointers. On a 64-bit system multiply that number by 8 to arrive at the
number of bytes of overhead (halve this on a 32-bit system). The overhead is
doubled when using a case insensitive filesystem.

The ideal size from a time complexity perspective is the number of files in your
largest directory. From a space complexity perspective, the ideal size is 1; you
would pay the cost of the collisions during the initial crawl and have a more
optimal memory usage. Since watchman is primarily employed as an accelerator,
we'd recommend biasing towards using more memory and taking less time to run.

### hint_num_dirs

_Since 4.6_

Used to pre-size hash tables that are used to track the total set of files in
the entire watched tree. The default value for this is 131072.

The optimal size is a power-of-two larger than the number of directories in your
tree; running `find . -type d | wc -l` will tell you the number that you have.

Making this number too large is potentially wasteful of memory. Making this
number too small results in increased latency during crawling while the hash
tables are rebuilt.

### suppress_recrawl_warnings

_Since 4.7_

When set to `true`, watchman will not produce recrawl related warning fields in
the response PDUs of various requests. The default is `false`; the intent is
that someone in your organization should be aware of recrawls and be able to
manage the configuration and workload. Some sites employ an alternative
mechanism for sampling and reporting this to the right set of people and wish to
disable the warning so that it doesn't appear in front of users that are unable
to make the appropriate configuration changes for themselves.

### eden_file_count_threshold_for_fresh_instance

This is specific to the EdenFS watcher

When set to a non-zero value, Watchman will return a _fresh instance_ to since
queries/subscriptions if the number of changed files exceeds the configured
value. In particular, during large updates of the working copies, a lot of files
may have changed forcing both Watchman and EdenFS to fetch a significant amount
of metadata to answer these queries.

This behavior is only enabled if the query specifies the
`empty_on_fresh_instance` option or when this config is set to `0`. Default to
`10000`.

By default every change in the mount counts toward the threshold, including
changes the query would filter out. A query that sets
`scope_fresh_instance_threshold` to `true` counts only changes inside its
`relative_root` that are not ignored (`ignore_dirs`, or deep paths under
`ignore_vcs`). A client watching one directory of a large repository then gets
the changed files it asked for, not a fresh instance, when many files change
elsewhere in the repository. Because out-of-scope changes then no longer end the
query early, such a client should keep the clock each query returns, even when
it returns no files; otherwise every query re-reads the same changes. Clients can
check for the `scope_fresh_instance_threshold` capability; older versions ignore
the field.

### eden_scoped_fresh_instance_scan_limit

This is specific to the EdenFS watcher

Bounds the work of a `scope_fresh_instance_threshold` query that also sets
`empty_on_fresh_instance` (and while `eden_file_count_threshold_for_fresh_instance`
is non-zero): once more than this many changes, inside or outside the query's
scope, have been read from EdenFS, the query returns a _fresh instance_. The
count is of streamed entries, so a path reported again by each of several
checkouts in the range counts each time. Defaults to 10 times
`eden_file_count_threshold_for_fresh_instance`.
