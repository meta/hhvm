
:::info[Note]
This is a point-in-time snapshot of the API documentation from January 2026.
Going forward, we will not be maintaining a public copy of these references,
and recommend users to refer to the built-in signature helpers available in
the Hack LSP instead for complete and up-to-date information.
:::

Create a function reference to a global function




``` Hack
namespace HH;

function fun(
  string $func_name,
);
```




The global function ` fun('func_name') ` creates a reference to a global
function.




The parameter ` 'func_name' ` is a constant string with the full name of the
global function to reference.




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
$v = vec["Hello", " ", "World", "!"];

// Each line below prints "Hello World!"
Vec\map($v, fun('printf'));
Vec\map($v, $x ==> { printf($x); });
```




## Parameters




* ` string $func_name ` A constant string with the name of the global method, including namespace if required.




## Returns




- ` $func ` - A fully typed function reference to the global method.
<!-- HHAPIDOC -->
