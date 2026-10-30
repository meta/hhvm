# HIP: revisiting `require class` trait constraints

#### **Author:** Francesco Zappa Nardelli
#### **Status:** Proposal
#### **Last updated:** Sept 14, 2026\.  First proposed: August 31, 2026
#### **Related proposals:** [HIP: require class](https://docs.google.com/document/d/1JvJL3K0S_--PXtHkhMpB-racRzNNim1zSRDCUh_rUO8/edit?tab=t.0), [HIP: require this as](https://docs.google.com/document/d/1HPER8Amrw4M8qr80kc3Pv5CYqjDfKr1Iab1RtESH2d0)

## **TL;DR**

We propose to support  trait constraints of the form `require class C` when `C` is NOT  a final class. In this case, to preserve soundness, we give the constraint the same semantics as `require this as C`, in particular  assuming that `this <: C` instead of `this = C` when typechecking the trait.  For final required classes the behavior is unchanged.  In parallel, we suggest deprecating `require this as` constraints, as these can now be expressed by `require class`, simplifying the overall syntax of trait constraints..

## Feature motivation and description

The `require this as` trait requirements were designed to circumvent the limitation that `require class` can only be used with `final` classes.  Indeed when typechecking the elements of a trait that has a `require class C` trait requirement, the typechecker assumes that `this = C` and such assumption is unsound if `C` has subclasses (example in Appendix).

The `require this as` constraints avoid this pitfall assuming only `this <: C` ; as a consequence they allow the enclosing trait  to be used by class `C` or any of its subclasses.  This causes *some* overlap with `require extends` requirements, which force the trait to be used by a strict subclass of `C`., without being a replacement of different approximations of `parent`: `require extends C` can safely approximate `parent` with `C`, while `require this as` must pick the parent of `C`, assuming it exists.

To guide programmers Hack raises a warning whenever a trait with a `require this as C` constraint is not used by `C` but only by a subclass: unless the trait is used by `C` itself there is no good reason to prefer `require this as` over `require extends`.  This situation is however suboptimal and in this HIP we propose to:

1. **deprecate** the `require this as` syntax;
2. **allow** `require class` to be used with **final** and  **non-final** classes:
   1. if `C` is `final,`then behaviour is unchanged: a trait with a `require class C` constraint allows the typechecker to assume `this = C` when typechecking the class elements;
   2. if `C` is not `final`, then a trait with a `require class C` constraint assumes the weaker `this <: C` constraint, behaving under the hood as today's  `require this as C`;
3. if a trait has `require class C`, then only class `C` can use the trait;
   1. As a consequence, trait can safely access the `private`  elements of `C`; (independently of whether  `C` is `final` or not);
4. it is an **error** for a trait `T` to specify `require class C` if the class `C` does not use the trait `T`.

All the other design choices of `require class` stand:

* it can only be used with a concrete, non generic, class name;
* it may only occur in a trait;
* it is inherited transitively through trait use.

This greatly simplifies the landscape of trait requirements with no lack of expressivity: programmers should:

* use `require extends C` if the  trait is not used  by `C` but only by its subclasses;
* use `require class C` if the trait is used by `C`.

## Implementation notes

### Hack

1. It is not possible to decide at decl-parsing time if a `require class` constraint implies equality or subtype constraints; the decl provider (`folded_class.mli`)  however already provides two calls to query all the `require class` and `require this as` constraints of a folded class/trait:

  ```
    (** Projection out of all_ancestor_req_constraints_requirements *)
    val all_ancestor_req_class_requirements : t -> requirement list

    (** Projection out of all_ancestor_req_constraints_requirements *)
    val all_ancestor_req_this_as_requirements : t -> requirement list
   ```

  Resolving the semantics of `require class` according to if the required class is `final` can be implemented inside these functions \- these are not hot so I don't think the extra decl class get will incur in a measurable perf penalty.

2. To simplify deprecation of `require this as`, we propose to raise an error when a trait states `require this as C` and  class `C` does not use the trait itself, instead of a warning as it is now.  This will make deprecating `require this as` as easy as renaming all the occurrences into `require class`.


### HHVM support

Support is limited to revisiting the errors about incompatible/unsatisfied trait requirements raised at class-load time by hhvm, and at trait flattening by hhbbc.

##  Alternative designs

* Deprecate `require class` and allow `require this as` to reference `final` classes and behave as `require class`: since `require this as` was introduced to allow a trait to be referenced by a class or its subclasses, the overlap with `require extends` remains confusing.  Also, `require class` is a better name to stress that the trait should be used only by the given class.
* Deprecating `require extends` in favour of `require this as` is not an option due to the different approximation of `parent` (also, not a blocker but currently `require this as` does not support generic classes).


## Appendix

If `require class` trait constraints drop the `final` requirement without relaxing the `this = C` assumption, the following code would typecheck but fatal at runtime.

```
trait T {
  require class C;                    // assume this = C

  public function return_C(): this {  // ok under the assumption this = C
    return new C();
  }
}

class C { // currently rejected because C must be final, but if not...
  use T;
}

final class D extends C {}

function consume_d(D $_): void {}

<<__EntryPoint>>
function main(): void {
  consume_d((new D())->return_C());  // typechecks but fatals at run-time
}
```
