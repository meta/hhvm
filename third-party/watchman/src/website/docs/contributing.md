---
title: Contributing
category: Internals
---

If you're thinking of hacking on watchman we'd love to hear from you! Feel free
to use the GitHub issue tracker and pull requests discuss and submit code
changes.

We (Facebook) have to ask for a "Contributor License Agreement" from someone who
sends in a patch or code that we want to include in the codebase. This is a
legal requirement; a similar situation applies to Apache and other ASF projects.

If we ask you to fill out a CLA we'll direct you to
[our online CLA page](https://code.facebook.com/cla) where you can complete it
easily. We use the same form as the Apache CLA so that friction is minimal.

Facebook Open Source provides a Code of Conduct statement for all projects to
follow, to promote a welcoming and safe open source community. Please
[read the full text](https://code.facebook.com/codeofconduct) so that you can
understand what actions will and will not be tolerated.

### Getting Started

You need to be able to build watchman from source and run its test suite. You
will need:

- python
- automake
- autoconf
- cmake
- libtool (or glibtool on macOS)
- libpcre
- libfolly (only needed if building the cppclient library)
- nodejs (for fb-watchman)

The build time dependencies can be installed by running
`getdeps.py --install-deps`. This is run for you when you run `autogen.sh` in
the example below:

```bash
$ git clone https://github.com/meta/watchman.git
$ cd watchman
$ ./autogen.sh
$ make
```

After making a change, run the integration tests to make sure that things are
still working well before you submit your pull request:

```bash
$ make integration
```

We'll probably ask you to augment the test suite to cover the functionality that
you're adding or changing.

Please keep in mind that our versioning philosophy in Watchman is to provide an
_append only_ API. If you're changing functionality, we'll ask you to do so in
such a way that it won't break older clients of Watchman.

### Don't forget the docs

If you're changing or adding new functionality, we'll ask you to also update the
documentation. You will need `ruby` 2.0.0 or later to preview the documentation.

One time setup:

```bash
$ cd website
$ sudo gem install bundler
$ sudo bundler install
```

Then:

```bash
$ jekyll serve -w -t
```

This will print out a URL that you can open in your browser to preview your
documentation changes.

The source for the documentation is in the `website/_docs` dir in markdown
format.
