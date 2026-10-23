# Lesson 4: Making an IR optimization

## Lesson goals:

* Use profiling tools to find an optimization opportunity.
* Learn how to read printir tracing, the IR documentation, and JIT types.
* Improve IR generation to speed up casting collections (e.g. Vector) to arrays (e.g. vec).
* Move this code to "simplify", one of the IR-to-IR optimization routines, to make it more general.

---

## Step 0: Suboptimal code generation

In this step, we're going to hunt for a potential performance win.

Our main tool in this hunt is profiling. One way to do that is to run
[Linux perf ](https://perf.wiki.kernel.org/index.php/Main_Page)
on a machine with an HHVM webserver under load. HHVM even outputs a
"perf-PID.map" file that can be used to symbolize stack traces in JIT-ed Hack
code. That said, it's easiest to read perf's numbers for C++ helpers, and many
performance opportunities in HHVM come from optimizing C++ functions that are
used to implement the Hack runtime.

In a sample perf trace I took, I saw that ~0.2% CPU is spent doing
object-to-array-like casts (e.g. convObjToVec, convObjToDict, convObjToKeyset).

What makes these helpers a good potential target for optimization? First off,
there is some opportunity there: if we could eliminate most of the cost of
these helpers, it could be a 0.2% gCPU win. Second, the opportunity is
relatively easy to realize. It's difficult to implement general-purpose code to
cast an object to an array-like. That's why HHVM implements these casts by
calling a C++ helper. But after having read enough of web code written in Hack,
I can make an educated guess that most object-to-array casts are casts on a
collections objects. These objects are secretly backed by arrays, which makes
it possible for us to generate faster code to do the cast in this case.

When we have a potential perf idea, it's important to write a small test case
and check that the opportunity shows up. Let's put the following code into
~/php/convert.php:

```
<?hh
<<__EntryPoint>>
function main() {
  $x = Vector { 17, 34 };
  var_dump(vec($x));
}
```

Now, when we run this code with tracing enabled, we must do so at the highest
optimization level! If we don't, we might go down a rabbit hole trying to make
an optimization that is irrelevant for our production use case. To see how HHVM
compiles the code above, we have to run it with both HHBBC (the
bytecode-to-bytecode optimizer) and RetranslateAll (the two-phase JIT) enabled:

```
TRACE=printir:1 hphp/tools/hhvm_wrapper.php -c -r 2 ~/php/convert.php | less -RN
```

Then we have to jump to the TransOptimize translation for main. Here's the
relevant part:

```
 B4: [profCount=2] (preds B0)
    --- bc main(id 1078142128)@5, fp 0, fixupFP 0, spOff 1,  [profTrans=0]
    ColFromArray Vector
    (12) t5:Obj=HH\Vector = NewColFromArray<HH\Vector> vec(0x8018d4e0)=Vanilla
        Main:
        0x3280000e: mov $0x8018d4e0, %edi
        0x32800013: callq  0xa712ff0 <HPHP::collections::allocFromArrayVector(HPHP::ArrayData*)>
        0x32800018: mov %rax, %rbx
    --- bc main(id 1078142128)@24, fp 0, fixupFP 0, spOff 3,  [profTrans=0]
    CastVec
    (22) t6:Vec = ConvObjToVec t5:Obj=HH\Vector -> B5<Catch>
        Main:
        0x3280001b: mov %rbx, %rdi
        0x3280001e: callq  0xa260c00 <HPHP::jit::convObjToVecHelper(HPHP::ObjectData*)>
  -> B6
 B6: [profCount=2] (preds B4)
    (23) StStk<IRSPOff -4> t1:StkPtr, t6:Vec
        Main:
        0x32800023: movb  $0x17, -0x38(%rbp)
        0x32800027: movq  %rax, -0x40(%rbp)
```

The important thing here is that, even though we know that t5 is an HH\Vector
in instruction (22) ConvObjToVec, we still generate machine code for it that
makes a call to a C++ function convObjToVecHelper. We can do better!

---

## Understanding IR tracing

We last looked at the TRACE=printir:1 output in the first lesson. Since then,
we've learned enough about HHVM to tease its output apart in more detail. This
tracing is useful because it simultaneously displays three levels of code
representation:

1. The input Hack bytecode, [documented in bytecode.specification](../bytecode.specification)
2. HHVM intermediate representation, [documented in ir.specification](../ir.specification)
3. The output machine code, [documented in an x86 manual](https://www.felixcloutier.com/x86/)

Here's the Hack bytecode annotation for the first IR op above. First, we see
the function and bytecode offset where this bytecode came from, a sort of
annotation from the bytecode back to source code. Then, we see the bytecode's
name and immediates:

```
    --- bc main(id 1078142128)@5, fp 0, fixupFP 0, spOff 1,  [profTrans=0]
    ColFromArray Vector
```

Here's the IR op. Like the bytecode, the IR op takes immediates - the
parameters inside the angle brackets. Unlike the bytecode, the IR takes inputs
and outputs inline. HHVM's IR operates on
[static, single-assignment (SSA) temporary values](https://en.wikipedia.org/wiki/Static_single_assignment_form).
For this op, the input SSATmp is a constant, so we hide its ID; the output
SSATmp is not constant, so we show that it's "t5", of type "Obj=HH\Vector". At
the IR level, we're no longer implicitly operating on a stack machine - all
stack operations are explicit!

```
    (12) t5:Obj=HH\Vector = NewColFromArray<HH\Vector> vec(0x8018d4e0)=Vanilla
```

Here's the machine code that we generate for this IR op. Machine code can be
placed in multiple regions of memory, depending on whether we think blocks at
the machine code level are likely to be executed. If they are, we put the code
in "Main"; otherwise, we put it in "Cold" or "Frozen". The machine code for
this op is simple because the heavy lifting is done in C++:

```
        Main:
        0x3280000e: mov $0x8018d4e0, %edi
        0x32800013: callq  0xa712ff0 <HPHP::collections::allocFromArrayVector(HPHP::ArrayData*)>
        0x32800018: mov %rax, %rbx
```

After the ColFromArray bytecode comes the CastVec bytecode. We compile this
bytecode to two IR ops. ConvObjToVec is doing the main logic of the Cast op,
and, again, is implemented mostly in C++. StStk ("store stack") pushes the
final result of the cast onto the stack. Unlike the other IR ops here, StStk is
compiled directly to machine code. It's simple enough that we don't need to
shell out to C++ for it.

## Where is the rest of the code?

If we look at the bytecode for these operations, we'll see that the IR trace
above skipped several bytecodes. You can see for yourself by running this
command, which is like our command above, except that:

* We're adding in "grep -v srcloc" to filter out source location attribution.
* We drop `TRACE=printir:1` and add `--hdf Eval.DumpHhas=1` to get a bytecode rather than an IR dump.
* We drop `-r 2` (which expands to `--retranslate-all 2`) because RetranslateAll results in more optimized IR generation - it doesn't affect the bytecode!

```
hphp/tools/hhvm_wrapper.php -c --hdf Eval.DumpHhas=1 ~/php/convert.php | grep -v srcloc
```

Here's what I get if I run that command:

```
.function{} [unique persistent "__EntryPoint"("""v:0:{}""")] (4,7) <"" N  > main() {
  .declvars _0;
  Vec @A_0
  ColFromArray Vector
  AssertRATL _0 Uninit
  AssertRATStk 0 Obj=HH\Vector
  PopL _0
  NullUninit
  NullUninit
  AssertRATL _0 Obj=HH\Vector
  PushL _0
  CastVec
  FCallFuncD <SkipRepack SkipCoeffectsCheck> 1 1 "" "" - "" "var_dump"
  AssertRATStk 0 InitNull
  PopC
  Null
  AssertRATL _0 Uninit
  RetC
}
```

In the bytecode, in between creating the Vector and casting it to a vec, we
store it to local 0 (via PopL), then unset the local and push it back onto the
stack (via PushL). Check bytecode.specification to confirm your understanding!
There are also some type assertion bytecodes (AssertRATL, "assert
repo-authoritative type for a local", and AssertRATStk, for the stack), but it
makes sense that these bytecodes don't compile to machine code. They just add
in type information at compile time.

It makes sense that the bytecode stores the Vector to a local - after all, that's what the source code does! The question is, why don't the IR ops match the bytecode above exactly? If the IR were to implement the exact same operations that the bytecode above claims to do, then we'd have several IR ops for each bytecode in this sequence:

1. **ColFromArray** would do the NewColFromArray, then use StStk to push that value onto the stack.
2. **PopL** would use LdStk ("load stack") to load that value into an SSATmp, then use StLoc ("store local") to store it to a local. In general, PopL also needs to load and dec-ref the old value of the local first, but in this case, the **AssertRATL** above tells us that the local is already Uninit, which is not a refcounted type.
3. **PushL** would use LdLoc ("load local") to load the value, then use StStk push it onto the stack. It would use StLoc to store an Uninit value to the local to "unset" it.
4. **CastVec** would use LdStk to read that stack value, do the cast, and then use StStk to push the result.

> Compiler engineers love abbreviations! "Loads" and "stores" are just reads
> and writes to memory. Because interacting with memory is ubiquitous in Hack
> code, we have tons of IR ops that use "Ld" or "St" in their names. We also
> use shorthand for many other ops, like CreateAFWH, DecRefNZ, JmpZ, and Shr.
> These names seem opaque at first, but they make reading code easier once you
> are versed in the domain.
> [If you're ever confused about what an IR op does, consult ir.specification.](../ir.specification)

These are sensible and correct translations of the bytecode above into IR.
They're also wasteful. The ColFromArray, PopL, PushL, and CastVec are pushing
and popping a single Vector value. If the net effect of all of these ops is to
do a cast on the Vector's SSATmp, it's better if we just do that and skip the
intermediate stack and local stores and loads. HHVM produces this more
optimized code in two steps:

1. It does a literal translation of each bytecode to IR, exactly as we did above.
2. Then, it analyzes the resulting IR ops and sees which ones can be optimized away.

There are actually two places we do the analysis needed to make these
optimizations. First, the irgen ("IR generation") code for each bytecode uses a
[forward-only analysis called FrameState](../../runtime/vm/jit/frame-state.h),
which eliminates operations loads that can be trivially removed (e.g. a LdStk
that comes right after a StStk). FrameState can eliminate redundant loads in
straight-line code, but we need to apply a more complex fixed-point algorithm
to handle branches and loops.
[load-elim](../../runtime/vm/jit/load-elim.cpp) and [store-elim](../../runtime/vm/jit/store-elim.cpp)
do these optimizations in general, after irgen is complete.

## Reading the IR spec

[ir.specification](../ir.specification) is actually a kind of executable
documentation. We use [this script](../generate-ir-opcodes.sh)
to process the lines beginning with `|` in this file to produce C++ code
defining a table of IR ops, the single source of truth for the IR. Because the
documentation is the implementation, it's always up to date! Let's look at a
few examples:

```
| ConvObjToVec, D(Vec), S(Obj), PRc|CRc

| StStk<offset>, ND, S(StkPtr) S(Cell), NF

| CheckType<T>, DRefineS(0), S(Cell), B|P
```

The first line says that ConvObToVec is an IR op taking one SSATmp input that
must be a subtype of TObj, and returning a subtype of TVec. The last bit of
that line defines the op's "flags" - in this case, it consumes a refcount on
the input and produces one on the output. That is: this IR op takes care of the
inc-ref and dec-ref required by the CastVec bytecode.
[For a complete list of flags, look arlier in the file.](../ir.specification?lines=119-159)

The second line says that StStk takes two inputs - a stack pointer and an
arbitrary Hack value (Cell == "mixed") - and doesn't return anything (ND == "no
destination"). That should make sense, as StStk simply writes to memory. StStk
doesn't have any additional flags, but it does have an immediate: the stack
offset, which is a constant offset off of the stack pointer. (Recall that an
immediate is an argument to an instruction that is "immediately" available at
compile time, as opposed to inputs, which are only available at runtime.) The
immediates in ir.specification are just comments; we define the immediates for
each op in code [at the end of the file extra-data.h](../../runtime/vm/jit/extra-data.h).
If you look for StStk there, you'll see that it takes an immediate of type
IRSPRelOffsetData.

The third line says that CheckType takes one input, an arbitrary Hack value,
and returns an output. The return type of this op isn't fixed; instead, it must
be computed at compile time based on the input, and based on the immediate type
T that a given CheckType is checking for. For example, we might compile a
CheckType<Bool> op, which takes a Cell input, checks that it's a bool, and
returns a refined SSATmp of type TBool. If the runtime type check fails, then
instead of returning the result, we'd branch (the "B" flag) to a "taken" block.

## The JIT type system

A key difference between Hack bytecode and HHVM's IR is that each IR op places
type constraints on its inputs, and produces typed outputs. The "CastVec"
bytecode will take any type of input from the stack and "do what vec cast does"
for that input. That could mean leaving the input unchanged (if it's a vec),
doing a cast (if it's a dict, keyset, or object), or throwing an error (all
other types). By contrast, the ConvObjToVec IR op will only implement vec
cast behavior for object inputs. It's up to our irgen code to only generate
IR ops whose type constraints are satisfied.

> In debug builds, after completing irgen for some block of code, we have
> assertions to check these type constraints. The only invariants that hold in
> HHVM are the ones that we actively check in debug mode!

We've already seen some examples of JIT types in the sources and destinations
of each op in ir.specification. These examples are instances of the C++ class
jit::Type, defined in [hphp/runtime/vm/jit/type.h](../../runtime/vm/jit/type.h).
A JIT type constraints what a Hack value could be at runtime. Unlike the Hack
type system, the JIT type system is sound, modulo bugs in HHVM: it provides
actual guarantees about runtime values, and without these guarantees, we
wouldn't be able to produce any nontrivial machine code.

Each instance of a JIT type represents a subset of values that match that type.
For example:

* TCell matches "any Hack value" - that is, an int, an array-like, an object, etc.
* TInt matches "any Hack integer" - some int, but we don't know which.
* TInt=5 matches "the integer 5"

That's right - JIT types can be constants! Like most compilers, HHVM does
[constant folding](https://en.wikipedia.org/wiki/Constant_folding): if an IR
instruction has constant inputs, and if it's side effect free, HHVM may compute
its output at compile time. We check if an IR instruction can be const-folded
by checking if all of that instruction's source SSATmps have types of constant
values.

> Based on the previous lesson, on refcounting, you may have a guess as to why
> we could have constant subtypes of TInt, TStr, TVec, etc., but not of TObj.
> There are even some types, like TInitNull, which always represent a constant!

The set of all JIT types forms [a mathematical structure called a lattice](https://en.wikipedia.org/wiki/Lattice_(order)).
That means that JIT types support two key operations: union and intersection.
The union `|` of two JIT types A and B is the type of values that could be an A
or a B. The intersection `&` of A and B is the type of values that must be both
an A and a B. Here are a few examples:

```
TCell | TInt = TCell   // A TCell could be any Hack value, so this union could be, too.
TCell & TInt = TInt    // A TCell and an int must be an int.
TStr & TInt  = TBottom // No value is an int and a string. TBottom is "the empty set".

TInt | TStr           = (Int|Str)          // jit::Type can represent this union...
(Int|Str) | TInitNull = (Int|Str|InitNull) // ...and this one.

// jit::Type can represent any union of Hack DataTypes (i.e. "top-level" Hack types)!

TInt=3 | TInt=5 = TInt    // We can't represent two constants, so we expand this union.
TInt=3 & TInt=5 = TBottom // No value is ever both the int 3 and the int 5.
```

We use the lattice operations to bound the type of values at compile time:

* **Unions represent merged control flow.** If we compile an if statement, and
  we assign local $x a value of type X in the "then" block and a value of type
  Y in the "else" block, then afterwards, $x could be either one. It has type X | Y.
* **Intersections represent type checks.** If some SSATmp has the type X, and
  we do a CheckType<Y> on it, then if the check passes, both type constraints
  apply. The CheckType result has type X & Y.

There's a lot more to learn about this type system, but it's best done by
reading code. Luckily, jit::Type is one of the most heavily-tested parts of
HHVM. In addition to exercising this code via the end-to-end tests in the
`hphp/test` directory, we have
[a battery of unit tests which double as examples of usage and expected behavior](../../runtime/test/type.cpp).

---

## Step 1: Optimizing CastVec IR generation

Phew! That was a lot of information. Take five and step away from the computer.
Go for a walk! Get a cup of coffee!

...but before you do, kick off a build of HHVM =)

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

It's time to apply what we've learned to optimize the example from the start of
this lesson - a simple vec cast on a Vector. At a high level, here's what we
have to do:

1. We want to improve irgen for CastVec. By convention, we know that the
   interpreter's logic for this bytecode is in iopCastVec and that the JIT's is
   in emitCastVec. **We must edit emitCastVec.**
2. Our optimization can only kick in if we have the right type info for the
   input. **We must check if the stack's top value is a subtype of
   TObj<=HH\Vector**, the specialized jit::Type representing objects of class
   Vector.
3. We need to use better - more specialized! - IR ops that will be more
   performant for this cast case. **We must find and compose existing IR ops,
   e.g. one that fetches the vec that backs a Vector.**

Let's go through these items together. For item 1, we can simply search in the
`hphp/runtime` directory for "emitCastVec". [Here's the code that I found.](../../runtime/vm/jit/irgen-basic.cpp)
At the rev you're working at, the code may be a bit different, but the
high-level structure is probably similar:

```
void emitCastVec(IRGS& env) {
  auto const src = popC(env);
  auto const raise = [&](const char* type) {
    auto const message =
      makeStaticString(fmt::format("{} to vec conversion", type));
    gen(env, ThrowInvalidOperation, cns(env, message));
    return cns(env, TBottom);
  };
  push(
    env,
    [&] {
      if (src->isA(TVec))     return src;
      if (src->isA(TArrLike)) return gen(env, ConvArrLikeToVec, src);
      if (src->isA(TClsMeth)) return raise("ClsMeth");
      if (src->isA(TObj))     return gen(env, ConvObjToVec, src);
      if (src->isA(TNull))    return raise("Null");
      if (src->isA(TBool))    return raise("Bool");
      if (src->isA(TInt))     return raise("Int");
      if (src->isA(TDbl))     return raise("Double");
      if (src->isA(TStr))     return raise("String");
      if (src->isA(TFunc))    return raise("Func");
      if (src->isA(TRes))     return raise("Resource");
      PUNT(CastVecUnknown);
    }()
  );
}
```

What's going on here? The C++ lambda inside the "push" call returns an SSATmp
that's the result of doing a cast. TVec, TArrLike, TObj, etc. are all JIT
types; most of them represent values of some DataType, but TArrLike is a
special union type that just means - you guessed it! - TVec | TDict | TKeyset.
In the TVec, TArrLike, and TObj cases, we produce IR to actually do the cast.
In all of the other cases, here, the cast is guaranteed to throw, so we emit a
"terminal" (the flag "T" in ir.specification) op `ThrowInvalidOperation`. This
op is guaranteed to halt execution, so we can return a dummy SSATmp of type
TBottom, since we know that value is unreachable.

> When compiling a bytecode to IR, it's critical to keep track of refcounting.
> This bytecode pops one value - the "src" SSATmp - from the stack, and pushes
> one value - the return value of the lambda - onto it. Based on the standard
> refcounting semantics, we must dec-ref the stack input and inc-ref the
> output.But we don't see any DecRef or IncRef IR ops here! That's because the
> ConvArrLikeToVec and ConvObjToVec IR ops handle the refcounting for us. We
> can confirm that by reading their ir.specification entries. The CRc|PRc flags
> on these ops mean that they consume a refcount on their input and produce one
> on their output. If we replace these ops with ones that don't handle
> refcounting, we'll have to generate refcounting IR ops ourselves.

For item 2, we must check the conditions under which our optimization applies.
Since we're trying to optimize vec casts on Vector objects, we only have to
consider the TObj case, but we can't apply our optimization to all objects. We
must use the JIT type system to check for a more specific type here.

Let's make this change by editing the TObj line alone. We can insert another
C++ lambda to give us some syntactic space to make this logic more complex.
Then, we can use the jit::Type's specialized constructor, jit::Type::SubObj, to
produce a type to compare against here:

```
      if (src->isA(TObj))     return [&]{
        auto const TVector = Type::SubObj(c_Vector::classof());
        if (src->isA(TVector)) {
          // TODO: We can optimize this case!
        }
        return gen(env, ConvObjToVec, src);
      }();
```

> We create and immediately invoke a closure here as a way to pack complex
> control flow in an expression position. It's the same reason that we create
> and invoke the outer closure, the one that produces the output that we push
> onto the stack. You could consider this pattern a "cute" "trick", or you
> could refactor this code to use named helper methods instead. The choice is
> yours!

Make these edits and check that HHVM still compiles. (You may need to include
"hphp/runtime/ext/collections/ext_collections-vector.h" at the top of the file,
too.) On to item 3! We need to find an IR op to extract the vec that backs a
vector. There are a few ways to look for this op:

* We could do a (case-insensitive) search for "collection" or "vector" in the file.
* We could look for ops that take an object as input, by searching for S(Obj).
* We could look for ops that return a vec as output, by searching for D(Vec).

The first approach quickly leads us to:

```
| LdColVec, D(Vec), S(Obj), NF

  Load the vec array backing a collection instance in S0. S0 must be a Vector
  or ImmVector, and that specific object type must be known at compile time.
```

That's exactly what we need! However, we now have to generate the appropriate
refcounting IR ops. Unlike ConvObjToVec, LdColVec has no special flags - it
simply does a load. In order to generate any IR ops here, we need to use the
magical templated "gen" helper. This helper takes a variadic inputs and builds
and IR instruction from those inputs. In order, its inputs are:

1. The IRGS& environment struct "env".
2. The IR op for the new instruction, specified by its name. (It's a C++ enum.)
3. (Optional) The "taken" branch, a jit::Block to jump to. Only used for control-flow-y ops (with flags "B" or "T").
4. (Optional) The jit::Type for the op. Only used if the op takes a type immediate, like CheckType does.
5. (Optional) Any other immediates associated with that op. (See extra-data.h to see if each op takes one.)
6. The list of source SSATmps for that op, in order.

To get this optimization working, we must gen an LdColVec, an IncRef of the
result, and a DecRef of the input. LdColVec and IncRef don't need any of the
three optional arguments above. DecRef has an associated immediate, but no
other optional arguments. As a result, this code should work for us:

```
      if (src->isA(TObj))     return [&]{
        auto const TVector = Type::SubObj(c_Vector::classof());
        if (src->isA(TVector)) {
          auto const result = gen(env, LdColVec, src);
          gen(env, IncRef, result);
          gen(env, DecRef, DecRefData(), src);
          return result;
        }
        return gen(env, ConvObjToVec, src);
      }();
```

> It's important that we do the IncRef here before the DecRef. Make sure you understand why!

When you've got this code compiling, rerun the TRACE=printir:1 command with the
test file and your new HHVM. You should see that the C++ call in ConvObjToVec
has been replaced with an offset load. Our code generation for this case is
much improved!

---

## Step 2: Moving the optimization to simplify

When writing a compiler optimization, you should always try to make it as
general as possible. If your optimization only applies in certain special
cases, then users of your language may see large performance differences due to
small edits to their code, which is a frustrating experience! Further,
performance measurement can be noisy, and the more cases your optimization
covers, the greater the chance of measuring an unambiguous improvement on real
benchmarks.

> Compiler engineers are greedy. They want to optimize as many cases as
> possible. Compiler engineers are also lazy. They may give up at the point at
> which it becomes hard to extend an optimization further. As a result of these
> warring impulses, compilers exhibit stepwise behavior. Ubiquitous, simple
> cases are heavily optimized. A variety of common cases are handled somewhat
> well. The general case may be slow as molasses.

There are a variety of ways to extend this optimization. We'll look at simpler
extensions in the exercises, but the most important extension here is to take
advantage of late type information.

[HHVM includes a variety of optimization passes.](../../runtime/vm/jit/opt.cpp)
These passes run after irgen is complete. They take an IR unit that implements
some Hack source code, and modify it in place to produce better IR that
implements the same code. These passes can produce tighter type bounds on
values than we originally had at irgen time; for example, if we inline a
function, we may get a better type for its return value, which recursively
results in tighter types for SSATmps computed based on that value.

If we can do an IR optimization without introducing new control flow, we can
take advantage of these late types by [moving the optimization to simplify.cpp](../../runtime/vm/jit/simplify.cpp).
The simplifier is run on every IR op after every optimization pass. If there's
any opportunity to make the optimization, the simplifier will find it. The
simplifier's contract is, uh, simple...it processes a single IR instruction,
and if it's possible to replace it with zero or more IR instructions, it does
so.

> When can we simplify an operation down to zero IR instructions? When we can
> const-fold it! Take a look at simplifyXorInt for an example. One of the cases
> we optimize here is xor-ing an SSATmp with itself. If we do that, we can
> return cns(env, 0). Since this result is a constant SSATmp, we don't need to
> execute any IR instructions to compute it. The simplifier will remove the
> XorInt op and replace its result with the constant.

To move the optimization to the simplifier, we need to:

1. **Revert the change in the irgen code.** The simplify version is strictly
   more general. If we leave the irgen change around, then when we examine the
   printir, it'll be hard to tell which of our changes kicked in.
2. **Add a simplifyConvObjToVec function to simplify.cpp.** We won't be able to
   const-fold here, but based on the input's type, we can still replace this IR
   instruction with a more optimized sequence of instructions.
3. **Register the IR op in the "X" macro below.** Search for "XorInt" in the
   file. It should appear in a few places, but one line in particular will just
   say "X(XorInt)". This line is a macro invocation that dispatches to the new
   simplify function.

Here's what I've got for the simplify code (as a diff: D34433352):

```
SSATmp* simplifyConvObjToVec(State& env, const IRInstruction* inst) {
  auto const src = inst->src(0);
  auto const TVector = Type::SubObj(c_Vector::classof());
  if (src->isA(TVector)) {
    auto const result = gen(env, LdColVec, src);
    gen(env, IncRef, result);
    gen(env, DecRef, DecRefData(), src);
    return result;
  }
  return nullptr;
}
```

It's basically the same as the irgen version, but by doing it in the right
place, we optimize more cases! Make this change, get it to compile, and verify
that you can see the optimization on the test case. Then kick off diff testing
and benchmarking!

---

## Lesson summary

* HHVM's IR is typed under the sound JIT type system. This type system
  constrains which IR operations we can use on a given value, but we can also
  use these types as optimization conditions.
* We use profiling tools to find "hot" (high-cost) functions that may be good
  targets for optimization.
* Once we've found a potential optimization target, we create a small test case
  and verify that the opportunity still exists at the highest optimization
  level.
* When we make an optimization, we should apply it to as many cases as
  possible. One way to make an optimization more general is to move it to the
  simplifier, so it runs whenever we gain type information.

---

## Exercises

Your first exercise is to think about how else we could extend this optimization. Do so before reading on!

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

For all of these extensions, be sure to create a small test case that shows the current, suboptimal code generation first. Then make the optimization and confirm that the test case is improved!

1. Modify the optimization to apply to vec-cast on ImmVector as well as vec-cast on Vector.
2. Add optimizations for doing a dict cast on a Map, ImmMap, Set, and ImmSet.
3. Add optimizations for doing a vec cast on a Map, ImmMap, Set, or ImmSet, and for doing a dict cast on a Vector or ImmVector. (Look for other relevant IR ops, like the ones that convert between different array-likes. Watch out for refcounting!)
