## Directory Structure

There are over 1000 C++ files in HHVM's codebase, split into a number of
different directories. This is a rough overview of what lives where (all paths are under `hphp/`):

`compiler/`: The old parser and bytecode emitter. This is deprecated and is currently being removed; the new replacement is in `hack/src/hackc/`.

`doc/`: Documentation, of varying age and quality.

`hack/src/`: The Hack parser, typechecker, bytecode emitter, and various other Hack-related tools.

`hhbbc/`: HipHop Bytecode-to-Bytecode Compiler, our ahead-of-time static analysis program.

`hhvm/`: `main()` for the `hhvm` binary

`neo/`: Code to read `.hdf` files, used for configuration.

`parser/`: The parser grammar and interface.

`pch/`: Pre-compiled headers for MSVC (not currently supported).

`ppc64-asm/`: Code to read and write [ppc64](https://en.wikipedia.org/wiki/Ppc64) machine code.

`runtime/`: HHVM's runtime.\
`runtime/base/`: Runtime internals related to Hack types and values (strings, objects, etc...).\
`runtime/debugger/`: Support for debugging Hack programs at runtime.\
`runtime/ext/`: Extensions.\
`runtime/server/`: Built-in servers, both for web requests and other HHVM-specific types of requests.\
`runtime/test/`: C++ unit tests for various runtime components.\
`runtime/vm/`: Most of the internals of the VM. The distinction between `runtime/base/` and `runtime/vm` is a bit fuzzy.\
`runtime/vm/jit/`: HHVM's JIT compiler.\
`runtime/vm/verifier/`: The bytecode verifier.

`system/`: systemlib, a collection of Hack code that is embedded into `hhvm` and always available.

`test/`: Integration tests for the whole compiler pipeline, written in PHP and Hack.

`tools/`: Utility programs useful in the development and/or use of HHVM.\
`tools/benchmarks/`: Microbenchmarks. If you run these, take them with a pound of salt; we care much more about macrobenchmarks based on real-life workloads.\
`tools/gdb/`: Python scripts to assist in debugging `hhvm` in `gdb`.\
`tools/hfsort`: HFSort, the algorithm used to lay out functions in both `hhvm` and JIT-compiled code.\
`tools/tc-print/`: TCPrint, a tool to analyze the output of the JIT compiler.

`util/`: Functions and data structures used by HHVM that could be useful to other programs, and have no dependencies on other parts of HHVM.

`vixl/`: Code to read and write [AArch64](https://en.wikipedia.org/wiki/ARM_architecture#AArch64) machine code.

`zend/`: Code imported from the standard [PHP interpreter](https://github.com/php/php-src).
