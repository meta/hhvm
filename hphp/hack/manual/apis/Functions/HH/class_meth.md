
:::info[Note]
This is a point-in-time snapshot of the API documentation from January 2026.
Going forward, we will not be maintaining a public copy of these references,
and recommend users to refer to the built-in signature helpers available in
the Hack LSP instead for complete and up-to-date information.
:::

Create a function reference to a static method on a class




``` Hack
namespace HH;

function class_meth(
  string $cls_name,
  string $meth_name,
);
```




The global function ` class_meth('cls_name', 'meth_name') ` creates a reference
to a static method on the specified class.




To identify the class you can specify either a constant string containing a
fully qualified class name including namespace, or a class reference using
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
class C {
    public static function isOdd(int $i): bool { return $i % 2 == 1;}
}
$data = Vector { 1, 2, 3 };

// Each result returns Vector { 1, 3 }
$data->filter(class_meth('C', 'isOdd'));
$data->filter(class_meth(C::class, 'isOdd'));
$data->filter($n ==> { return C::isOdd($n); });
```




## Parameters




* ` string $cls_name ` A constant string with the name of the class, or
  a class reference using `` FullClassName::class ``.
* ` string $meth_name ` A constant string with the name of the static class method.




## Returns




- ` $func_ref ` - A fully typed function reference to the static class method.
<!-- HHAPIDOC -->
