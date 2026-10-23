# Lesson 0: Building and running HHVM

## Lesson goals

* Understand what HHVM is and what it can be used for
* Set up a devserver and build HHVM
* Run HHVM on a small standalone Hack file, and examine its output

---

## Step 0: Starting a build

Kick off the following steps before proceeding through the guide. They take
time, so we want to get them started now.

> As performance engineers, we're always looking for ways to optimize our lives =)

1. [Consult the main documentation for system requirements.](https://docs.hhvm.com/hhvm/installation/building-from-source)
2. [Check out the HHVM repository.](https://docs.hhvm.com/hhvm/installation/building-from-source#downloading-the-hhvm-source-code)
3. [Kick off a build of HHVM.](https://docs.hhvm.com/hhvm/installation/building-from-source#building-hhvm)

---

## What is HHVM?

HHVM is a compiler and runtime for the dynamically-typed language Hack.

Hack is a variant of PHP. Like PHP, Hack is a good language for writing the
backend of a web application. Large websites are written in Hack, including
[facebook.com](https://www.facebook.com/).

## What are our goals for HHVM?

We'd like to improve HHVM in two main ways:

1. **Better performance:** By optimizing the machine code that HHVM generates,
   we can realize significant capacity wins for people who are deploying code
   and websites built in Hack.
2. **Language improvements:** We work with the Hack language team to improve
   the language itself. These improvements include changes that make Hack
   simpler and safer (e.g. [Hack Array Migration](https://hhvm.com/blog/10649/improving-arrays-in-hack))
   as well as new language features (e.g. [readonly](https://hhvm.com/blog/2022/02/22/announcing-readonly.html)).

> Performance wins in HHVM can save our users lots of money. That's why there
> are so many $$$ in Hack code!

## How does HHVM work?

HHVM has two strategies for executing Hack code:

* [An interpreter:](https://en.wikipedia.org/wiki/Interpreter_(computing))
  basically, a while loop that executes Hack code step-by-step.
* [A just-in-time (JIT) compiler:](https://en.wikipedia.org/wiki/JIT_compilation)
  a compiler that can compile a whole sequence of Hack code into optimized
  machine code, at runtime. Most webservers running HHVM use the JIT to compile
  code while serving web requests.

> Let's say that one more time, because it's a bit strange and magical! **HHVM
> is a JIT compiler, which means that it can compile Hack code while executing
> that code.** We'll discuss this idea at length later.

HHVM's interpreter and JIT are interoperable; to execute a given web request,
we might run some JIT-ed code for some Hack functions and use the interpreter
for others. JIT-ed code is typically 10-50x faster than interpreted code. Our
aim is to maximize overall performance, measured as webserver throughput. To
do so, we preferentially JIT the "hottest" (most expensive) Hack functions.
In typical usage, we may only JIT a small fraction (say 1%) of a web codebase,
but that may be enough coverage for us to spend ~99% of our time in JIT-ed code
and only ~1% of time in the interpreter.

---

## Step 1: Running HHVM

Since you've completed step 0 already ;) you should have an HHVM binary sitting
in your build directory. Let's poke it. Here's what mine says if I ask for its
version. Yours will have a different compiler ID, repo schema, etc. The
compiler ID should match the git hash at which you compiled HHVM:

```
$ hg log -r . -T{node}
e0e849921332a9ccacf5d06a6ad05b87bc0ae0ba
$ ./build/hhvm --version
HipHop VM 4.141.0-dev (dbg) (lowptr)
Compiler: default-0-ge0e849921332a9ccacf5d06a6ad05b87bc0ae0ba
Repo schema: 3eabd3c8d14119f88cbf4ade871a893128d72200
```

Let's compile and run a simple PHP file. Create a directory called ~/php -
that's where all of our code examples will live! - and then put the following
code into ~/php/hello-world.php:

```
<?hh

<<__NEVER_INLINE>>
function add($a, $b) {
    return $a + $b;
}

<<__EntryPoint>>
function main() {
  var_dump(add(7, 10));
  var_dump(add(7.0, 10.0));
}
```

We're going to use a script called "hhvm_wrapper" to run HHVM on this file.
This script is unnecessary for simply running a file, but it makes it much
easier to run HHVM in various compilation modes that mimic its production
behavior, and to see debugging output. As a result, we will **always use
hhvm_wrapper** in our examples. Here's a simple invocation:


```
$ hphp/tools/hhvm_wrapper.php ~/php/hello-world.php
int(17)
float(17)
```

Okay, that's good. HHVM can do simple arithmetic correctly...phew!

What's more interesting about this example is that we called add once with two
integers, and once with two floating-point values. Integers and floats have
completely different representations at the hardware level, and we need to use
different machine code instructions (and even different arithmetic circuitry)
to do math on these two types. And yet, HHVM somehow compiles machine code for
"add" that works for both cases. How does that work?

Well, what we do is...

...

...

...

...

...

...

...

...

...

...

...

...

...

...

...

...


Nope! The answer to that question is way beyond the scope of this lesson.

---

## Step 2: Down the rabbit hole

What we can offer you, for now, is a glimpse into what HHVM is doing behind the
scenes. Let's run hhvm_wrapper in two more modes. First off, let's look at
HHVM's bytecode representation. We'll add the flag`--hdf Eval.DumpHhas=1` to
enable bytecode output. This flag disable code executions, so we'll only see
the bytecode, no results:

```
$ hphp/tools/hhvm_wrapper.php --hdf Eval.DumpHhas=1 ~/php/hello-world.php
```

There's no guarantee that you'll get exactly what I got, but if you focus on
the bytecode for the "add" function, you might see something like the following
output:

```
.function{} ["__NEVER_INLINE"("""v:0:{}""")] (4,6) <"" N > add($a, $b) {
  .srcloc 5:15,5:16;
  CGetL $b
  CGetL2 $a
  Add
  .srcloc 5:3,5:17;
  RetC
}
```

We're doing something with locals $a and $b - in a funny order! - and then
we're executing the "Add" bytecode. That makes sense, right? The most important
takeaway from this small test case is that Hack bytecode is untyped. When we
compile Hack source code to bytecode, we don't know the types of Hack values,
so we don't emit type-specific code. The "Add" bytecode takes two inputs and
does "whatever + does in Hack" for those inputs.

> Hack is a dynamic language, and we can write functions - like this "add"
> function, or like "Vec\map" - that operate on many different types of inputs.
> As a result, **Hack bytecode (usually) does not contain type information.**

Now, let's take a look at the next steps of compilation: an intermediate
representation (IR) and the resulting machine code. Let's add a few flags to
our original command:

* `TRACE=printir:1` will cause us to print both IR and machine code.
* `-r 2` will make HHVM run the code twice, then recompile the code in a more optimized way.
* `| less -RN` is just a Unix pager for this output. There's gonna be a lot of output!

```
$ TRACE=printir:1 hphp/tools/hhvm_wrapper.php -r 2 ~/php/hello-world.php | less -RN
```

Don't be intimidated by the volume of output here! Focus on the colors. Aren't
they pretty?

Okay, let's examine a tiny piece of this code. Search for "TransOptimize" in
this output. (In less, you can use "/" to search. That means you should type
"/TransOptimize", then press Enter. Then, you can type "n" to jump to the next
instance of that string, and "N" to jump back to the previous one.)

We're searching for "TransOptimize" because we're only looking for optimized
code generation; unoptimized output looks very, very different. The first
optimized function you'll find is probably "main". You can tell where you are
because the function is labeled (in purple, for me) near that header - I see,
"Function main" (after the digraph output). Keep jumping to the next instance
of "TransOptimize" until you find one for "add". There should be two compiled
sections for this function - one for integers, and one for floats!

> HHVM's JIT automatically specializes functions for the most common input
> types to that function.

Can you tell which compilations of "add" correspond to integers, and which to
floats? Hints:

* Look at the "LdLoc" - that is, "load a local" - and "CheckType" - that is, "check a value's type" - operations that appear early in each compilation of "add".
* Remember that "Dbl", or "double", is a 64-bit float.

---

## Lesson summary

* HHVM is a just-in-time or JIT compiler for the dynamically-typed programming language, Hack.
* A JIT compiler can compile code at runtime, and can choose which functions to compile and which to interpret.
* You can use hhvm_wrapper to execute a given Hack file, or to look at intermediate steps of compiler output.

---

## Exercises

Let's modify our "hello-world.php" script to investigate Hack bytecode and HHVM's IR.

1. If we change "add" to "return $a + 2 * $b", how does its bytecode change?
   Which arithmetic operation is done first?
2. With this modified "add" function, how do we implement the "times 2" bit for
   integer $b? Look at the printir!
3. Suppose, instead, we modify main to include these two additional lines. How
   many TransOptimize compilations do we now generate for the "add" function?
   Does this number make sense? Why?

```
var_dump(add(7, 10.0));
var_dump(add(7.0, 10));
```

Now, let's go back to the original version of "hello-world.php", and look at
the printir trace for "main", instead.

1. How many function calls do we make in the TransOptimize compilation for "main"?
2. If we remove the `<<__NEVER_INLINE>>` hint on "add", how does the
   TransOptimize compilation for "main" change? How many function calls do we
   make there now?
