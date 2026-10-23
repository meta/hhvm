# FAQ

This document contains answers to some of the most common questions we get from
people as they learn HHVM's codebase. Some of the answers may be pointers to
other pieces of documentation, if that information has a natural home somewhere
else.

## How is HHVM configured?

HHVM has a large number of configuration options that can be set with
command-line arguments of the form `-vName=Value`. Unfortunately, documentation
on them is very sparse, but some can be found in
[doc/options.compiled](../options.compiled) and
[doc/options.compiler](../options.compiler). The authoritative source for core
runtime options is the `RuntimeOption` class, in
[runtime/base/runtime-option.h](../../runtime/base/runtime-option.h) and
[runtime/base/runtime-option.cpp](../../runtime/base/runtime-option.cpp).

Multiple options may be collected in a `.hdf` file, [described here](../hdf),
and passed to HHVM with `-c path/to/config.hdf`. Options given with `-v` will
override any found in the `.hdf`. HHVM also has support for PHP's `.ini` files,
documented [here](../ini.md).

## How do I see the bytecode for my Hack/PHP script?

Use the `Eval.DumpBytecode` runtime option:

```sh
$ hhvm -vEval.DumpBytecode=1 hello.php
```

This will give you output that looks something like
[this](https://gist.github.com/swtaarrs/991c37af6e474733c47911731521a8ec).
[bytecode.specification](../bytecode.specification) contains descriptions of the
behavior of every bytecode instruction. Note that running with
`-vEval.DumpBytecode=1` will still execute the script after printing its
bytecode. To prevent this, add the `--lint` option, which will exit after
parsing and printing the bytecode.

You can also use `Eval.DumpHhas`:

```sh
$ hhvm -vEval.DumpHhas=1 hello.php
```

This gives output similar to
[this](https://gist.github.com/swtaarrs/4b2fffacd74c31d4e65298888922805d).
Running with `-vEval.DumpHhas=1` will *not* execute the script after printing
its bytecode.

Why are there two ways to dump bytecode with slightly different semantics? It's
partly for historical reasons that may be cleaned up at some point, but there
are a few meaningful differences between the two:
- `DumpBytecode` shows bytecode offsets, while `DumpHhas` doesn't. This is because HHAS is designed to be human-writable, so it uses labels rather than raw offsets for control flow. However, the offsets are useful if you're working in the interpreter or JIT, as they use bytecode offsets to locate code.
- `DumpBytecode` writes to the current `TRACE` file (see the next section for details), while `DumpHhas` writes to stdout.
- Metadata is printed explicitly, separately from function bodies with `DumpBytecode`, while `DumpHhas` either has the same metadata represented inline in the bytecode, or left out when it can be inferred from the bytecode itself.

In general, if you're working with HackC (the parser and bytecode emitter), you
should use `DumpHhas`, because that's how HHVM and HackC communicate. If you're
working with HHVM's interpreter or JIT, you should use `DumpBytecode`, since its
more verbose format can be helpful during debugging.

## How do I enable debug logging from different parts of HHVM?

The primary method of printing debug information is our tracing system, defined
in [util/trace.h](../../util/trace.h). Most of the functionality is disabled in
release builds for performance reasons, so you should be using a debug build if
you want to enable tracing.

### Basic usage

The `TRACE_MODULES` macro in `trace.h` defines a series of *trace modules*, each
with a short name. To enable one or more modules, set the `TRACE` environment
variable to a comma-separated list of `module:level` pairs. All levels default
to 0, and higher levels are typically more verbose (this is only by convention,
and is not enforced anywhere). For example, to set the `hhir` module to level 1
and the `printir` module to level 2, run:

```sh
$ TRACE=hhir:1,printir:2 hhvm my_script.php
```

If `TRACE` is set, even to an empty value, tracing is written to
`/tmp/hphp.log`; this can be overridden by setting `HPHP_TRACE_FILE` to the file
of your choice. If `TRACE` is not set, all tracing goes to stderr (this applies
to features like `Eval.DumpBytecode`, described above). To send tracing to
stderr even when `TRACE` is set, use `HPHP_TRACE_FILE=/dev/stderr`.

To find out which trace module applies to a specific file, look for a use of the
`TRACE_SET_MOD` macro, usually near the top of the file. To add your own tracing
lines, use either `TRACE(level, "printf-style format", args...);` or
`FTRACE(level, "fmt::format-style format", args...);`. The `level` argument is
the lowest level at which the trace will be active.

### `printir`

If you're working in the JIT, `printir` is probably the most important module to
be familiar with. It controls the printing of HHIR units as they're constructed
and optimized, and `TRACE=printir:1` is the most convenient way to see the
output of the JIT. It's also one of the only modules to have well-defined
meanings for each trace level, defined
[here](https://github.com/facebook/hhvm/blob/38ee69496f66e87528a128e22c38e2ee12da5470/hphp/runtime/vm/jit/print.h#L76-L101).
It is enabled in all build modes, so you don't have to create a debug build to
use it.

## My change is crashing HHVM. What do I do?

HHVM can be debugged with the standard debugging tools for your compiler and
platform. For most of us, that means using
[GDB](https://www.gnu.org/software/gdb/):

```sh
$ gdb --args ./hhvm path/to/script.php
```

There are a variety of support scripts in [tools/gdb](../../tools/gdb) to help
with inspecting many of HHVM's data structures; see the README in that directory
for more information on usage.

# Common Pitfalls

This section contains answers to questions that most people don't think to ask,
usually because they involve some non-obvious or surprising behavior in HHVM.

## `jit::Type`

If you spend any time in the JIT, you'll probably deal with the
[`jit::Type`](../../runtime/vm/jit/type.h) struct. [HHIR](../ir.specification)
is statically typed, so every value is tagged with a `Type` and every
instruction has a set of acceptable `Type`s for its inputs. If you're used to
types in languages like Java or C++, `Type` is much more complicated than
something like `int` or `Foo*`. We recommend reading about it
[here](jit-core.md#type-system) before writing any nontrivial HHIR code. You
don't have to memorize every single `Type`, but you should make sure you
understand the [usage guidelines](jit-core.md#usage-guidelines).
