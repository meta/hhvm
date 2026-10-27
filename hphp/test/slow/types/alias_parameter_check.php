<?hh

// Running this test multiple times with profiling enabled (see the .config file)
// tests whether instance bits are implemented properly in the presence of type
// aliases.

interface Base {}
type Foo = Base;
class Bar implements Base {}

function doit(Foo $x) :mixed{
  var_dump($x is Foo);
}

function main() :mixed{
  $x = new Bar;
  doit($x);
}


<<__EntryPoint>>
function main_alias_parameter_check() :mixed{
main();
}
