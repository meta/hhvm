---
title: C++ Client
category: Invocation
sidebar_position: 5
---

_Since 4.8._

Watchman includes a C++ client library to facilitate easy access to Watchman
data from C++ applications. This library provides APIs for:

- Opening and maintaining a connection to a local Watchman server.
- Executing request-response Watchman commands.
- Subscribing to updates with in directory trees.

## Installation

Provided the Folly library is present when Watchman is built, the C++ client
library is automatically built and installed. For details on building Watchman
see [Installation](install.md).

## API

The public Watchman C++ client API is entirely covered in the installed
`watchman/WatchmanClient.h` header file. This header contains a usage synopsis
and notes on the public API features.

For a simple example of API usage, sending simple request-response commands to
Watchman, see `cppclient/CLI.cpp` in the Watchman source tree. For a more
extensive example of the API including use of subscriptions, see the integration
test `integration/cppclient.cpp` also in the Watchman source.

The C++ client library and its API make heavy use of the Folly library and as
such familiarity with this is highly recommended. Specifically, the client
library makes extensive use of
[Folly's async features](https://github.com/meta/folly/blob/main/folly/io/async/README.md)
to provide high-performance asynchronous I/O, and
[Folly's dynamics](https://github.com/meta/folly/blob/main/folly/docs/Dynamic.md)
to avoid needing to construct/process raw JSON in C++.

## Using the C++ client in your application's build

To facilitate integration into your application's build, the Watchman C++ client
library provides support for
[pkg-config](https://www.freedesktop.org/wiki/Software/pkg-config/).

For example, if your application was contained entirely in one C++ file called
`app.cpp` the following would be sufficient for build on a system with GNU Make:

```bash
$ make LDFLAGS=$(pkg-config watchmanclient --libs) CPPFLAGS=$(pkg-config watchmanclient --cflags) app
```

If Watchman is installed in a location `pkg-config` does not search for packages
by default, you may need to modify the `PKG_CONFIG_PATH` environment variable.
For example:

```bash
$ export PKG_CONFIG_PATH=<watchman path>/lib/pkgconfig:$PKG_CONFIG_PATH
```
