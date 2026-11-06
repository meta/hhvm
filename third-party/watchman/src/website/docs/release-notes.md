---
title: Release Notes
category: Installation
---

Watchman is continuously deployed inside Facebook, which means that we don't
explicitly maintain version numbers. We have automation that cuts a weekly tag
with a named derived from the date. You can learn more about how to reason about
supported _capabilities_ and our backwards compatibility guidelines in the
[Compatibility Rules](compatibility.md) docs.

We focus on the highlights only in these release notes. For a full history that
includes all of the gory details, please see
[the commit history on GitHub](https://github.com/meta/watchman/commits/main).

### Watchman v2020.07.13.00

- Added script `watchman-replicate-subscription`. It can replicate an existing
  watchman subscription. Integrators can use this script to validate watchman
  notifications their client is receiving.
- Added support for suffix sets in suffix expressions. You now can specify
  multiple suffixes to match against by setting the second argument to a list of
  suffixes. See `suffix-set` documentation for
  [more details](expr/suffix.md#suffix-set)
- pywatchman: introduced new pywatchman_aio client for python
- Windows: we no longer trust environment variables to locate the state
  directory which should result in a better experience for users that mix
  cygwin, mingw, native windows and/or WSL or other environments
- Windows: we now support unix domain sockets on Windows 10. The CLI will prefer
  to use unix domain sockets when available.

We weren't great at updating the release notes since the prior release; there
was a lot of work to support our sister project EdenFS that isn't broadly
relevant to those outside FB at the time of writing.

### Watchman 4.9.0 (2017-08-24)

- New field: `content.sha1hex`. This field expands to the SHA1 hash of the file
  contents, expressed in hex digits (40 character hex string). Watchman
  maintains a cache of the content hashes and can compute the hash on demand and
  also heuristically as files are changed. This is useful for tooling that wants
  to perform more intelligent cache invalidation or build artifact fetching from
  content addressed storage.
- Experimental feature: Source Control Aware query mode. Currently supports only
  Mercurial (patches to add Git support are welcomed!). SCM aware query mode
  helps to keep response sizes closer to `O(what-you-changed)` than to
  `O(all-repo-changes)` when rebasing your code. Using this feature effectively
  may require some additional infrastructure to compute and associate data with
  revisions from your repo.
- Fixed an issue that resulted in the perf logging thread deadlocking when
  `perf_logger_command` is enabled in the global configuration
- Fixed an issue where queries larger than 1MB would likely result in a PDU
  error response.
- Reduced lock contention for subscriptions that do no use the advanced settling
  (`drop`, `defer`) options.
- Fixed `since` generator behavior when using unix timestamps rather than the
  preferred clock string syntax
- Improved the reporting of "new" files in watchman results
- Improved performance of handling changes on case insensitive filesystems
- Windows: promoted from alpha to beta status!
- Windows: fixed some performance and reliability issues
- Windows: now operates correctly on Windows 7
- Windows: can now see and report symlinks and junction points
- Windows: fixed potential deadlock in trigger deletion
- Windows: fixed stack trace rendering on win32
- Windows: improved IO scheduling around deletes on win32
- Windows: improved handling of case insensitive win32 driver letters
- pywatchman: the python wheel format is used for publishing watchman pypi
  package
- pywatchman: now watchman path is configurable in python client
- pywatchman: now python client can be used as a context manager
- Solaris: support for Solaris has been removed. If you'd like to commit to
  testing and maintaining Solaris support, we'd love to hear from you!

### Watchman 4.8.0 (never formally released)

Whoops, we never got around to tagging this beyond a release candidate tag!

- New command `flush-subscriptions` to synchronize subscriptions associated with
  the current session.
- On Windows, return `/` as the directory separator. Previously we used `\`.
  This change should be pretty neutral for clients, and makes it easier to work
  with both the internals and the integration test infrastructure.
- Enforce socket Unix groups more strongly — Watchman will now refuse to start
  if it couldn't gain the right group memberships, as can happen for sites that
  are experiencing intermittent LDAP connectivity problems.
- pywatchman now officially supports Python 3. pywatchman will return Unicode
  strings (possibly with surrogate escapes) by default, but can optionally
  return bytestrings. Note that on Python 3, pywatchman requires Watchman 4.8
  and above. The Python 2 interface and requirements remain unchanged.
- Prior to 4.8, methods on the Java WatchmanClient that returned
  ListenableFutures would swallow exceptions and hang in an unfinished state
  under situations like socket closure or thread death. This has been fixed, and
  now ListenableFutures propagate exception conditions immediately. (Note that
  this is typically unrecoverable, and users should create a new WatchmanClient
  to re-establish communication with Watchman.) See #412.
- The minimum Java version for the Watchman Java client has always been 1.7, but
  it was incorrectly described to be 1.6. The Java client's build file has been
  fixed accordingly.
- Watchman was converted from C to C++. The conversion exposed several
  concurrency bugs, all of which have now been fixed.
- Subscription queries are now executed in the context of the client thread,
  which means that subscriptions are dispatched in parallel. Previously,
  subscriptions would be serially dispatched and block the disk IO thread.
- Triggers are now dispatched in parallel and waits are managed in their own
  threads (one thread per trigger). This improves concurrency and resolves a
  couple of waitpid related issues where watchman may not reap spawned children
  in a timely fashion, or may spin on CPU until another child is spawned.
- Fixed an object lifecycle management issue that could cause a crash when aging
  out old/transient files.
- Implement an upgraded wire protocol, BSERv2, on the server and in pywatchman.
  BSERv2 can carry information about string encoding over the wire. This lets
  pywatchman convert to Unicode strings on Python 3. Clients and servers know
  how to transparently fall back to BSERv1.
- OS X: we no longer use socket activation when registering with launchd. This
  was the source of some upgrade problems for mac Homebrew users.

### Watchman 4.7.0 (2016-09-10)

- Reduced memory usage by 40%
- Queries can now run with a shared lock. It is recommended that clients move
  away from the `n:FOO` style server side named cursor clockspecs to take full
  advantage of this.
- Added new `glob` generator as a walking strategy for queries. This allows
  watchman to evaluate globs in the most efficient manner. Our friends in the
  Buck project have already integrated this into their `BUCK` file parsing to
  evaluate globs without touching the filesystem!
- Added `"case_sensitive": true` option to queries to force matches to happen in
  a case sensitive manner, even if the watched root is on a case insensitive
  filesystem. This is used to accelerate certain types of internal traversal: if
  we know that a path is case sensitive we can perform an `O(1)` lookup where we
  would otherwise have to perform an `O(number-of-directory-entries)` scan and
  compare.
- Fixed a race condition during subscription initiation that could emit
  incorrect clock values.
- Fixed spurious over-notification for parent directories of changed files on
  Mac.
- Fixed some reliability issues on Windows

### Watchman 4.6.0 (2016-07-09)

- Improved I/O scheduling when processing recursive deletes and deep directory
  rename operations.
- Improved performance of the `ignore_dirs` configuration option on OS X and
  Windows systems. We take advantage of an undocumented (but supported!) API to
  further accelerate this for the first 8 entries in the `ignore_dirs` on OS X.
  Users that depend on this configuration to avoid recrawls will want to review
  and prioritize their most active build dirs to the front of the `ignore_dirs`
  specified in their `.watchmanconfig` file.
- Added an optional recrawl recovery strategy for OS X that will attempt to
  resync from the fseventsd journal rather than performing a full filesystem
  walk. This is currently disabled by default but will likely be enabled by
  default in the next Watchman release. You can enable this by setting
  `fsevents_try_resync: true` in either `/etc/watchman.json` or your
  `.watchmanconfig`. This should reduce the frequency of recrawl warnings for
  some users/workloads, and also improves I/O for users with extremely large
  trees.
- Fixed accidental exponential time complexity issue with recursive deletes and
  deep directory rename operations on case-insensitive filesystems (such as OS
  X). This manifested as high CPU utilization for extended periods of time.
- Added support for allowing non-owner access to a Watchman instance. Only the
  owner is authorized to create or delete watches. Non-owners can view
  information about existing watches. Access control is based on unix domain
  socket permissions. The new but not yet documented configuration options
  `sock_group` and `sock_access` can be used to control this new behavior.
- Added support for inetd-style socket activation of the watchman service.
  [this commit includes a sample configuration for systemd](https://github.com/meta/watchman/commit/2985377eaf8c8538b28fae9add061b67991a87c2).
- Added the `symlink_target` field to the stored metadata for files. This holds
  the text of the symbolic link for symlinks. You can test whether it is
  supported by a watchman server using the capability name
  `field-symlink_target`.
- Fixed an issue where watchman may not reap child processes spawned by
  triggers.
- Fixed an issue where watchman may block forever during shutdown if there are
  other connected clients.
- Added `hint_num_dirs` configuration option.

### pywatchman 1.4.0 (????-??-??)

(These changes have not yet been released to pypi)

- Added immutable version of data results to bser. This is cheaper to build from
  a serialized bser representation than the mutable version and is better suited
  to large result sets received from watchman.
- Fixed a number of misc. portability issues
- Added Python 3.x support

### Watchman 4.5.0 (2016-02-18)

- Fixed an inotify race condition for non-atomic directory replacements that was
  introduced in Watchman 4.4.

### Watchman 4.4.0 (2016-02-02)

- Added state-enter and state-leave commands can allow subscribers to more
  intelligently settle/coalesce events around hg update or builds.
- Fixed an issue where subscriptions could double-notify for the same events.
- Fixed an issue where subscriptions that never match any files add
  O(all-observed-files) CPU cost to every subscription dispatch

### Watchman 4.3.0 (2015-12-14)

- Improved handling of case insensitive renames; halved the memory usage and
  doubled crawl speed on OS X.

### Watchman 4.2.0 (2015-12-08)

- Increased strictness of checks for symlinks; rather than just checking whether
  the leaf of a directory tree is a symlink, we now check each component down
  from the root of the watch. This improves detection and processing for
  directory-to-symlink (and vice versa) transitions.
- Increased priority of the watchman process on OS X.

### pywatchman 1.3.0 (2015-10-22)

- Added `watchman-make` and `watchman-wait` commands
- Added pure python implementation of BSER

### Watchman 4.1.0 (2015-10-20)

- Fixed an issue where symlink size was always reported as 0 on OS X using the
  new bulkstat functionality

### Watchman 4.0.0 (2015-10-19)

- Fixed an issue where a directory that was replaced by a symlink would cause a
  symlink traversal instead of correctly updating the type of the node and
  marking the children removed.
- Fixed a debugging log line that was emitted at the wrong log level on every
  directory traversal.

### Watchman 3.9.0 (2015-10-12)

- Fixed an issue where dir renames on OS X could cause us to lose track of the
  files inside the renamed dir
- Fixed an issue where dir deletes and replacements on Linux could cause us to
  lose track of the files inside the replaced dir (similar to the OS X issue
  above in manifestation, but a different root cause).
- Improved (re)crawl speed for dirs with more than a couple of entries on
  average (improvement can be up to 5x for dirs with up to 64 entries on
  average). You may now tune the `hint_num_files_per_dir` setting in your
  `.watchmanconfig` to better match your tree.
  [More details](config.md#hint_num_files_per_dir)
- Improved (re)crawl speed on OS X 10.10 and later by using `getattrlistbulk`.
  This allows us to improve the data:syscall ratio during crawling and can
  improve throughput by up to 40% for larger trees.
- Add optional `sync_timeout` to the `clock` command
- Avoid accidentally passing descriptors other than the stdio streams when we
  spawn the watchman service.
- Fixed a race condition where we could start two sets of watcher threads for
  the same dir if two clients issue a `watch` or `watch-project` at the same
  time
- Added a helpful error for a tmux + launchd issue on OS X

### Watchman 3.8.0 (2015-09-14)

- Improved latency of processing kernel notifications. It should now be far less
  likely to run into an notification queue overflow.
- Improved idle behavior. There were a couple of places where watchman would
  wake up more often than was strictly needed and these have now been fixed.
  This is mostly of interest to laptop users on battery power.
- Improved inotify move tracking. Some move operations could cause watchman to
  become confused and trigger a recrawl. This has now been resolved.
- Hardened statedir and permissions. There was a possibility of a symlink attack
  and this has now been mitigated by re-structuring the statedir layout.
- Fixed a possible deadlock in the idle watch reaper
- Fixed an issue where the watchman -p log-level debug could drop log
  notifications in the CLI
- Disabled the IO-throttling-during-crawl that we added in 3.7. It proved to be
  more harmful than beneficial.
- `-j` CLI option now accepts either JSON or BSER encoded command on stdin
- Added [capabilities](capabilities.md) to the server, and added the
  [capabilityCheck](cmd/version.md#capabilityCheck) method to the python and
  node clients.

### pywatchman 1.2.0 (2015-08-15)

- Added the `capabilityCheck` method
- Added `SocketTimeout` exception to distinguish timeouts from protocol level
  exceptions

### fb-watchman 1.3.0 for node (2015-08-15)

- Added the [capabilityCheck](nodejs.md#checking-for-watchman-availability)
  method.

### pywatchman 1.0.0 (2015-08-06)

- First official pypi release, thanks to [@kwlzn](https://github.com/kwlzn) for
  setting up the release machinery for this.

### Watchman 3.7.0 (2015-08-05)

(Watchman 3.6.0 wasn't formally released)

- Fixed bug where `query match` on `foo*.java` with `wholename` scope would
  incorrectly match `foo/bar/baz.java`.
- Added `src/**/*.java` recursive glob pattern support to `query match`.
- Added options dictionary to `query`'s `match` operator.
- Added `includedotfiles` option to `query match` to include files whose names
  start with `.`.
- Added `noescape` option to `query match` to make `\` match literal `\`.
- We'll now automatically age out and stop watches. See
  [idle_reap_age_seconds](config.md#idle_reap_age_seconds) for more information.
- `watch-project` will now try harder to re-use an existing watch and avoid
  creating an overlapping watch.
- Reduce I/O priority during crawling on systems that support this
- Fixed issue with the `long long` data type in the python BSER module

### fb-watchman 1.2.0 for node (2015-07-11)

- Updated the node client to more gracefully handle `undefined` values in
  objects when serializing them; we now omit keys whose values are `undefined`
  rather than throw an exception.

### Watchman 3.5.0 (2015-06-29)

- Fix the version number reported by watchman.

### Watchman 3.4.0 (2015-06-29)

- `trigger` now supports an optional `relative_root` argument. The trigger is
  evaluated with respect to this subdirectory. See
  [trigger](cmd/trigger.md#relative-roots) for more.

### fb-watchman 1.1.0 for node (2015-06-25)

- Updated the node client to handle 64-bit integer values using the
  [node-int64](https://www.npmjs.com/package/node-int64). These are most likely
  to show up if your query fields include `size` and you have files larger than
  2GB in your watched root.

### fb-watchman 1.0.0 for node (2015-06-23)

- Updated the node client to support [BSER](bser.md) encoding, fixing a
  quadratic performance issue in the JSON stream decoder that was used
  previously.

### Watchman 3.3.0 (2015-06-22)

- `query` and `subscribe` now support an optional `relative_root` argument.
  Inputs and outputs are evaluated with respect to this subdirectory. See
  [File Queries](file-query.md#relative-roots) for more.
