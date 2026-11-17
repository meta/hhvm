---
title: Installation
category: Installation
---

## System Requirements

Watchman is known to compile and pass its test suite on:

- <i class="fa fa-linux"></i> Linux systems with `inotify`
- <i class="fa fa-apple"></i> macOS (uses `FSEvents` on 10.7+, `kqueue(2)` on
  earlier versions)
- <i class="fa fa-windows"></i> Windows 10 (64-bit) and up. Windows 7 support is
  provided by community patches

Watchman used to support the following systems, but no one is actively
maintaining them. The core of the code should be OK, but they likely don't
build. We'd love it if someone would step forward to maintain them:

- BSDish systems (FreeBSD 9.1, OpenBSD 5.2) that have the `kqueue(2)` facility
- Illumos and Solaris style systems that have `port_create(3C)`

Watchman relies on the operating system facilities for file notification, which
means that you will likely have very poor results using it on any kind of remote
or distributed filesystem.

Watchman does not currently support any other operating system not covered by
the list above.

## Windows

### Prebuilt Binaries

1. Download and extract the windows release from the
   [latest release](https://github.com/meta/watchman/releases/latest)
2. It will be named something like `watchman-vYYYY.MM.DD.00-windows.zip`
3. It contains a `bin` folder. Move that somewhere appropriate and update your
   `PATH` environment to reference that location.

Watchman needs the latest
[Microsoft Visual C++ Redistributable](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)
for x64; with an older one it can crash when it starts.

The `bin` folder includes the Python that `watchman-wait`, `watchman-make`,
`watchman-replicate-subscription` and `watchman-diag` run on. With `bin` on
`PATH`, programs that look for `python3.dll` there, such as Vim's `:py3`, load
that Python instead of yours; set Vim's `pythonthreedll` to the full path of
your own Python's `pythonXY.dll` to avoid this.

If you encounter issues with the Windows version of watchman, please report them
via GitHub!
[You can find the list of known Windows issues here](https://github.com/meta/watchman/issues?utf8=%E2%9C%93&q=is%3Aopen+Windows).

### Installing via Chocolatey

Watchman is available via the
[Chocolatey](https://community.chocolatey.org/packages/watchman) Windows package
manager. Installation is as simple as:

```powershell
PS C:\> choco install watchman
```

The package is maintained by the community rather than by Meta, so if you
experience issues with installation or uninstallation, you should
[contact the package maintainers](https://chocolatey.org/packages/watchman/ContactOwners)
for assistance.

## macOS

### <a name="homebrew-instructions"></a> Homebrew

Homebrew's [Watchman package](https://formulae.brew.sh/formula/watchman#default)
is community-maintained, but it works well for many.

```sh
$ brew update
$ brew install watchman
```

If for some reason you can't wait for the Homebrew package to update, you can
install the latest build from GitHub:

```sh
$ brew install --HEAD watchman
```

### <a name="macports"></a> MacPorts

To install the package maintained by
[MacPorts](https://ports.macports.org/port/watchman/):

```bash
$ sudo port install watchman
```

### Prebuilt Binaries

The macOS release runs on Apple Silicon Macs with macOS 15 or newer.

1. Download and extract the macOS release from the
   [latest release](https://github.com/meta/watchman/releases/latest)
2. It will be named something like `watchman-vYYYY.MM.DD.00-macos.zip`

```bash
$ unzip watchman-vYYYY.MM.DD.00-macos.zip
$ cd watchman-vYYYY.MM.DD.00-macos
$ xattr -dr com.apple.quarantine bin
$ sudo mkdir -p /usr/local/bin /usr/local/var/run/watchman
$ sudo cp bin/* /usr/local/bin
$ sudo chmod 755 /usr/local/bin/watchman
$ sudo chmod 2777 /usr/local/var/run/watchman
```

The Watchman binaries are not notarized by Apple, so macOS won't run them while
they have the quarantine attribute that browsers add to downloads. The `xattr`
command removes it.

## Linux

### Homebrew

If you use Homebrew on Linux, it's a great way to get a recent Watchman build.

Follow the [macOS instructions above](#homebrew-instructions).

### Fedora and RHEL (Prebuilt RPM)

**Warning**: Do not install the Fedora-supplied Watchman package. It is old and
missing security, bug, and performance fixes.

The .rpm works on Fedora 40, RHEL 10 and newer.

1. From the
   [latest release](https://github.com/meta/watchman/releases/latest),
   download the .rpm for your CPU: `x86_64`, or `aarch64` for 64-bit ARM
2. `sudo dnf install ./watchman-$VERSION-1.x86_64.rpm` (or `.aarch64.rpm`)
3. Confirm successful installation by running `watchman version`

### Ubuntu and Debian (Prebuilt Deb)

**Warning**: Do not install the Ubuntu-supplied Watchman package. It is old and
missing security, bug, and performance fixes.

The .deb works on Ubuntu 24.04, Debian 13 and newer.

1. From the
   [latest release](https://github.com/meta/watchman/releases/latest),
   download the .deb for your CPU: `amd64`, or `arm64` for 64-bit ARM
2. `sudo apt install ./watchman_vYYYY.MM.DD.00_amd64.deb` (or `_arm64.deb`)
3. Confirm successful installation by running `watchman version`

### Prebuilt Binaries

**Note**: Our binaries are built from the main branch only. We don't provide
binaries for v4.9.0.

**Note**: The Linux binaries are compiled on a GitHub Action VM (ubuntu-24.04 at
the time of this writing). They link every library except glibc and the GCC
runtime statically, so they run on distributions with glibc 2.39 and the
libstdc++ of GCC 13 or newer, such as Ubuntu 24.04, Debian 13, Fedora 40 and
RHEL 10.

Watchman is continuously deployed as it passes our internal test validation
inside Meta and doesn't use manually assigned or "approved" version numbers.

Outside Meta we have automation that cuts a tag and builds binaries on Monday of
each week and assigns a tag based on the date. That process is in a beta state;
some or all of the binaries may not be present for any given tag.

1. Download and extract the release for your system from the
   [latest release](https://github.com/meta/watchman/releases/latest)
2. It will be named something like `watchman-vYYYY.MM.DD.00-linux.zip`, or
   `watchman-vYYYY.MM.DD.00-linux-arm64.zip` for 64-bit ARM

```bash
$ unzip watchman-vYYYY.MM.DD.00-linux.zip  # or watchman-vYYYY.MM.DD.00-linux-arm64.zip
$ cd watchman-vYYYY.MM.DD.00-linux  # or watchman-vYYYY.MM.DD.00-linux-arm64
$ sudo mkdir -p /usr/local/bin /usr/local/var/run/watchman
$ sudo cp bin/* /usr/local/bin
$ sudo chmod 755 /usr/local/bin/watchman
$ sudo chmod 2777 /usr/local/var/run/watchman
```

## <a name="building-from-source"></a> Building from Source

You need the [Watchman repository](https://github.com/meta/watchman/) or a
[source snapshot from the latest release](https://github.com/meta/watchman/releases/latest).

The build needs CMake, Ninja, a C++20 compiler, Python 3 with setuptools, and
Cargo, either from your OS's package manager or https://rustup.rs/.

It uses git to download folly, fbthrift and the other Meta libraries that
Watchman uses from GitHub, and compiles them, so the first build takes a while.

### Linux

These are the packages for Ubuntu and Debian:

```bash
$ sudo apt-get install -y cmake g++ git ninja-build \
    libevent-dev libgflags-dev libpcre2-dev libsodium-dev libssl-dev \
    libxxhash-dev libzstd-dev python3-dev python3-setuptools zlib1g-dev
$ cd watchman
$ cmake -B _build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build _build
```

### macOS

```bash
$ brew install cmake gflags libevent libsodium ninja openssl@4 pcre2 pkgconf xxhash zstd
$ python3 -m venv _venv
$ _venv/bin/pip install setuptools
$ cd watchman
$ cmake -B _build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DPython3_EXECUTABLE="$PWD/../_venv/bin/python"
$ cmake --build _build
```

### Windows

The steps in
[windows.yml](https://github.com/meta/watchman/blob/main/.github/workflows/windows.yml)
build Watchman with Visual Studio 2022 and vcpkg.

### Testing and Installing

```bash
$ ctest --test-dir _build --output-on-failure -j 8
```

The build puts `watchman` in `_build/bin` and `watchmanctl` in
`_build/watchman/cli/release`. On Linux and macOS, install them the same way as
the prebuilt binaries:

```bash
$ sudo mkdir -p /usr/local/bin /usr/local/var/run/watchman
$ sudo cp _build/bin/watchman _build/watchman/cli/release/watchmanctl /usr/local/bin
$ sudo chmod 2777 /usr/local/var/run/watchman
```

A build from a source snapshot has no git history to take its version from. Add
`-DWATCHMAN_VERSION_OVERRIDE=YYYY.MM.DD.00` to the first `cmake` command,
otherwise `watchman --version` prints `0.0.0`.

## System Specific Preparation

### Linux inotify Limits

The `inotify(7)` subsystem has three important tunings that impact watchman.

- `/proc/sys/fs/inotify/max_user_instances` impacts how many different root dirs
  you can watch.
- `/proc/sys/fs/inotify/max_user_watches` impacts how many dirs you can watch
  across all watched roots.
- `/proc/sys/fs/inotify/max_queued_events` impacts how likely it is that your
  system will experience a notification overflow.

You obviously need to ensure that `max_user_instances` and `max_user_watches`
are set so that the system is capable of keeping track of your files.

`max_queued_events` is important to size correctly; if it is too small, the
kernel will drop events and watchman won't be able to report on them. Making
this value bigger reduces the risk of this happening.

Watchman has two simple strategies for mitigating an overflow of
`max_queued_events`:

- It uses a dedicated thread to consume kernel events as quickly as possible
- When the kernel reports an overflow, watchman will assume that all the files
  have been modified and will re-crawl the directory tree as though it had just
  started watching the dir.

This means that if an overflow does occur, you won't miss a legitimate change
notification, but instead will get spurious notifications for files that haven't
actually changed.

### macOS File Descriptor Limits

_Only applicable on macOS 10.6 and earlier_

The default per-process descriptor limit on macOS is extremely low (256!).

Watchman will attempt to raise its descriptor limit to match
`kern.maxfilesperproc` when it starts up, so you shouldn't need to mess with
`ulimit`; just raising the sysctl should do the trick.

The following will raise the limits to allow 10 million files total, with 1
million files per process until your next reboot.

```bash
$ sudo sysctl -w kern.maxfiles=10485760
$ sudo sysctl -w kern.maxfilesperproc=1048576
```

Putting the following into a file named `/etc/sysctl.conf` on macOS will cause
these values to persist across reboots:

```
kern.maxfiles=10485760
kern.maxfilesperproc=1048576
```
