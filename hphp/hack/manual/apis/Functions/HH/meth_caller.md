
:::info[Note]
This is a point-in-time snapshot of the API documentation from January 2026.
Going forward, we will not be maintaining a public copy of these references,
and recommend users to refer to the built-in signature helpers available in
the Hack LSP instead for complete and up-to-date information.
:::

Create a function reference to an instance method that can be called on any
instance of the same type




``` Hack
namespace HH;

function meth_caller(
  string $cls_name,
  string $meth_name,
);
```




The global function ` meth_caller('cls_name', 'meth_name') ` creates a reference
to an instance method on the specified class.  This method can then be used
to execute across a collection of objects of that class.




To identify the class for the function, use a class reference of the format
` MyClassName::class `.




Hack provides a variety of methods that allow you to construct references to
methods for delegation.  The methods in this group are:




+ [` class_meth `](/apis/Functions/HH/class_meth/) for static methods on a class
+ [` fun `](/apis/Functions/HH/fun/) for global functions
+ [` inst_meth `](/apis/Functions/HH/inst_meth/) for instance methods on a single object
+ [` meth_caller `](/apis/Functions/HH/meth_caller/) for an instance method where the instance will be determined later
+ Or use anonymous code within a [lambda](</hack/functions/anonymous-functions>) expression.




# Example




```
<?hh
$v = Vector { Vector { 1, 2, 3 }, Vector { 1, 2 }, Vector { 1 } };

// Each result returns Vector { 3, 2, 1 };
$result2 = $v->map(meth_caller(Vector::class, 'count'));
$result3 = $v->map($x ==> $x->count());
```




## Parameters




* ` string $cls_name ` A constant string with the name of the class, or
  a class reference using `` FullClassName::class ``.
* ` string $meth_name ` A constant string with the name of the instance method.




## Returns




- ` $func_ref ` - A fully typed function reference to the instance method.
<!-- HHAPIDOC -->
