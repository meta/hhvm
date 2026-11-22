# Hacker's Guide to HHVM

This directory contains documentation on the internal architecture of HHVM,
targeted at C++ developers looking to hack on HHVM itself. If you're a
Hack developer looking for documentation on using HHVM, that can be found
[here](https://docs.hhvm.com/).

HHVM is a [virtual
machine](https://en.wikipedia.org/wiki/Virtual_machine#Process_virtual_machines)
that executes [Hack](http://hacklang.org/) programs using a [bytecode
interpreter](https://en.wikipedia.org/wiki/Interpreter_(computing)#Bytecode_interpreters)
and a [JIT compiler](https://en.wikipedia.org/wiki/Just-in-time_compilation)
(the latter is vastly more complex, and will get much more airtime here).
[PHP](https://php.net) is also currently supported for historical reasons.

You should already be comfortable reading and writing C++ (specifically, HHVM is
written in [C++14](https://en.wikipedia.org/wiki/C%2B%2B14)), as well as
navigating around large codebases using
[grep](https://en.wikipedia.org/wiki/Grep),
[ctags](https://en.wikipedia.org/wiki/Ctags), or any similar tool of your
choice. Prior experience with compilers and runtimes will help but is not
strictly necessary.

## Code References

Since this guide is intended to help familiarize readers with the HHVM codebase,
it naturally contains a number of links to the code. Some of these links are to
the current version of a file, and others are to specific lines in specific
versions of files. If you find links of either kind that are out-of-date with
current `master` or are otherwise misleading, please let us know.

## Building HHVM

Instructions for building HHVM and running our primary test suite can be found
[here](https://docs.hhvm.com/hhvm/installation/building-from-source). The
initial build may take an hour or longer, depending on how fast your machine is.
Subsequent builds should be faster, as long as you're not touching core header
files.

## Architecture Overview

HHVM, like most compilers, is best thought of as a pipeline with many different
stages. Source code goes in one end, and after a number of different
transformations, executable machine code comes out the other end. Some stages
are optional, controlled by runtime options. Each stage is described in detail
in the articles listed below, but here's a quick-and-dirty overview:

Source code enters the compiler frontend and is converted to a token stream by
the [lexer](https://en.wikipedia.org/wiki/Lexical_analysis). The
[parser](https://en.wikipedia.org/wiki/Parsing#Computer_languages) reads this
token stream and converts that to an [abstract syntax
tree](https://en.wikipedia.org/wiki/Abstract_syntax_tree), or AST. This AST is
then converted to a stream of [bytecode
instructions](https://en.wikipedia.org/wiki/Bytecode) by the bytecode emitter.
Everything up to this point is written in Rust (via HackC); the rest of HHVM is written in
C++ and assembly.

After the bytecode and associated metadata are created, our bytecode optimizer,
HHBBC, is optionally run. Finally, the bytecode, optimized or not, is stored
into a `.hhbc` file we call a "repo".

If the frontend was invoked directly by HHVM, the bytecode also lives in-memory
in the `hhvm` process, and execution can begin right away. If the frontend was
invoked as an ahead-of-time build step, the bytecode will be loaded from the
repo by `hhvm` when it eventually starts. If the JIT is disabled, the bytecode
interpreter steps through the code one instruction at a time, decoding and
executing each one. Otherwise, the JIT is tasked with compiling the bytecode
into machine code.

The first step in the JIT is region selection, which decides how many bytecode
instructions to compile at once, based on a number of complicated heuristics.
The chosen bytecode region is then lowered to HHIR, our primary [intermediate
representation](https://en.wikipedia.org/wiki/Intermediate_representation). A
series of optimizations are run on the HHIR program, then it is lowered into
vasm, our low-level IR. More optimizations are run on the vasm program, followed
by register allocation, and finally, code generation. Once various metadata is
recorded and the code is relocated to the appropriate place, it is ready to be
executed.

## Getting Started

If you're not sure where to start, skimming these articles is a good first step:

* [Directory structure](directory-structure.md)
* [FAQ](faq.md)
* [Glossary](glossary.md)

## HHVM Internals

The articles in this section go into more detail about their respective
components:

* [HHBC spec](../bytecode.specification)
* Frontend
  * Parser
  * Emitter
  * HHBBC
  * ...
* VM Runtime
  * [Core data structures](data-structures.md)
    * [Hack-visible](data-structures.md#hack-visible-values)
      * [Datatype, Value, and TypedValue](data-structures.md#datatype-value-and-typedvalue)
      * [ArrayData](data-structures.md#arraydata)
      * [StringData](data-structures.md#stringdata)
      * [ObjectData](data-structures.md#objectdata)
      * [Smart pointer wrappers](data-structures.md#smart-pointer-wrappers)
    * [Runtime-internal](data-structures.md#runtime-internal-data-structures)
      * [Unit](data-structures.md#unit)
      * [PreClass/Class](data-structures.md#preclass-and-class)
      * [Func](data-structures.md#func)
  * [Memory management](memory-management.md)
  * Execution Context
  * [Bytecode interpreter](bytecode-interpreter.md)
  * Unwinder
  * Treadmill
  * Debugger
  * ...
* JIT Compiler
  * [Core concepts](jit-core.md)
  * [Optimization passes](jit-optimizations.md)
  * ...
