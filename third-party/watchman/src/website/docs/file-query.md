---
title: File Queries
category: Queries
---

Watchman file queries consist of 1 or more _generators_ that feed files through
the _expression evaluator_.

### Generators

Generators are analogous to the list of _paths_ that you specify when using the
`find(1)` utility, but are implemented in watchman with a bit of a twist because
watchman doesn't need to crawl the filesystem in realtime and instead maintains
a couple of indexes over the tree.

A query may specify any number of generators; each generator will emit its list
of files and this may mean that you see the same file output more than once if
you specified the use of multiple generators that all produce the same file.

Watchman provides 5 generators:

- **since**: produces a list of files that were modified since a specific
  clockspec.
- **suffix**: produces a list of files that have a particular suffix.
- **glob**: efficiently pattern match a list of files based on their names.
- **path**: produces a list of files based on their path and depth.
- **all**: produces a list of all known files

### De-duplicating results

_Since 4.7._

If your query uses multiple generators, or configures the `path` generator with
paths that yield multiple results, the default behavior (for backwards
compatibility reasons) is to emit those duplicate results in the query output.

You may ask Watchman to de-duplicate results for you by enabling the
`dedup_results` boolean in your query:

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "path": ["bar", "bar"],
  "dedup_results": true
}]
EOT
```

You may test for this feature using an extended version command and requesting
the capability name `dedup_results`.

### Since Generator

The `since` generator produces a list of files that were modified since a
specific [clockspec](clockspec.md).

The following query will consider the set of files changed since the last query
using the named cursor `mycursor` and then pass them to the expression evaluator
to be filtered to just those that are files:

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "since": "n:mycursor",
  "expression": ["type", "f"]
}]
EOT
```

If the `since` parameter value is blank, was produced by a different watchman
process (in other words, the watchman process was restarted between the time
that the value was obtained and the time the query was issued) or is a named
cursor that has not yet been used in a query, the `since` generator will
consider the state to be a _fresh instance_ and its behavior is modified:

A _fresh instance_ result set will only include files that currently exist and
will generate file nodes that are always considered to be `new`.

If the query was configured with the `empty_on_fresh_instance` property set to
`true` then the result set will be empty and the `is_fresh_instance` property
will be set to `true` in the result object.

On EdenFS, a clock from a different watchman process can resume instead of
producing a _fresh instance_; see
[eden_resume_other_process_clocks](config.md#eden_resume_other_process_clocks).

The since generator also knows how to talk to source control;
[you can read more about that here](scm-query.md).

The `since` generator does not consider the targets of symlinks. In particular,
the `since` generator may _not_ produce a symlink in the following cases:

- The symlink's target was a file, and the file is since modified.
- The symlink's target was a file, and the file is since deleted or replaced
  with a different file.
- An ancestor of the symlink's target was created or deleted or modified.
- The symlink's target was a directory, and a file is since added or removed
  from that directory.

### Suffix Generator

The `suffix` generator produces a list of files that have a particular suffix or
set of suffixes. The value can be either a string or an array of strings.

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "suffix": "js"
}]
EOT
```

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "suffix": ["js", "css"]
}]
EOT
```

If the `suffix` generator is given an empty array, it produces no files.

The `suffix` generator can produce symlinks.

The `suffix` generator does not follow symlinks. For example, a symlink to
`/etc` will not cause a `"suffix": "conf"` query to search within `/etc` and
produce `/etc/resolv.conf`.

### Glob Generator

_Since 4.7._

The `glob` generator produces a list of files by matching against your input
list of patterns. It does this by building a tree from the glob expression(s)
and walking both the expression and the in-memory filesystem tree concurrently.

This query will yield a list of all of the C source and header files found
directly in the `src` dir:

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "glob": ["src/*.c", "src/*.h"],
  "fields": ["name"]
}]
```

This query will yield a list of all of the C source and header files found in
any subdirectories of the root:

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "glob": ["**/*.c", "**/*.h"],
  "fields": ["name"]
}]
```

Note that it is more efficient to use the `suffix` generator together with a
`dirname` expression term for such a broadly scoped query as it results in fewer
comparisons. This example is included as an illustration of recursive globbing.

The glob generator implicitly enables `dedup_results` mode.

If the `glob` generator is given an empty array, it produces no files.

The `glob` generator can produce symlinks.

The `glob` generator does not follow symlinks. For example, a symlink to `/etc`
will not cause a `"glob": ["**/resolv.conf"]` query to search within `/etc` and
produce `/etc/resolv.conf`.

### Path Generator

The `path` generator produces a list of files based on their path and depth.
Depth controls how far watchman will search down the directory tree for files.

The `path` generator expects an array of path specifiers. Each path specifier
can be either a string or an object and each will produce a set of files.

If it is a string then it is treated as the value for `path` with `depth` set to
infinite. If an object, the fields `path` (a string) and `depth` (an integer)
must be supplied.

Paths are relative to the root, so if watchman is watching `/foo/`, path `bar`
refers to `/foo/bar`.

A `depth` value of `0` means only files and directories which are contained in
this path. A `depth` value of `-1` means no limit on the depth.

The following `path` generators are equivalent:

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "path": ["bar"]
}]
EOT
```

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
  "path": [{"path": "bar", "depth": -1}]
}]
EOT
```

If the `path` generator is given an empty array, it produces no files.

The `path` generator can produce symlinks.

The `path` generator does not follow symlinks.

### All Generator

The `all` generator produces a list of all file nodes. It is the default
generator and is used in the case where no other generators were explicitly
specified.

```bash
$ watchman -j <<-EOT
["query", "/path/to/root", {
}]
EOT
```

The `all` generator can produce symlinks.

The `all` generator does not follow symlinks.

### Expressions

A watchman query expression consists of 0 or more expression terms. If no terms
are provided then each file evaluated is considered a match (equivalent to
specifying a single `true` expression term).

Otherwise, the expression is evaluated against the file and produces a boolean
result. If that result is true then the file is considered a match and is added
to the output set.

An expression term is canonically represented as a JSON array whose zeroth
element is a string containing the term name.

```json
["termname", arg1, arg2]
```

If the term accepts no arguments you may use a short form that consists of just
the term name expressed as a string:

```json
"true"
```

Expressions that match against file names may match against either the
_basename_ or the _wholename_ of the file. The basename is the name of the file
within its containing directory. The wholename is the name of the file relative
to the watched root.

You can find a list of all possible expression terms in the sidebar on the left
of this page.

### Relative roots

_Since 3.3._

Watchman supports optionally evaluating queries with respect to a path within a
watched root. This is used with the `relative_root` parameter:

```json
["query", "/path/to/watched/root", {
  "relative_root": "project1",
}]
```

Setting a relative root results in the following modifications to queries:

- The `path` generator is evaluated with respect to the relative root. In the
  above example, `"path": ["dir"]` will return all files inside
  `/path/to/watched/root/project1/dir`.
- The input expression is evaluated with respect to the relative root. In the
  above example, `"expression": ["match", "dir/*.txt", "wholename"]` will return
  all files inside `/path/to/watched/root/project1/dir/` that match the glob
  `*.txt`.
- Paths inside the relative root are returned with the relative root stripped
  off. For example, a path `project1/dir/file.txt` would be returned as
  `dir/file.txt`.
- Paths outside the relative root are not returned.

Relative roots behave similarly to a separate Watchman watch on the
subdirectory, without any of the system overhead that that imposes. This is
useful for large repositories, where your script or tool is only interested in a
particular directory inside the repository.
