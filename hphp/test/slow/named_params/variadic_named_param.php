<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

// A `named t...` parameter has no name, so nothing here can read the arguments
// it absorbs. It only means such arguments are accepted instead of rejected.
function only_variadic(int $x, named arraykey...): void {
  var_dump($x);
}

// Mixed with real named params, some required, some optional.
function mixed_named(int $x, named int $b, named int $d = 40, named arraykey...): void {
  var_dump(vec[$x, $b, $d]);
}

// Absorbing names alongside a positional variadic.
function with_positional_variadic(int ...$args, named arraykey...): void {
  var_dump($args);
}

function only_positional_variadic(int ...$args): void {
  var_dump($args);
}

class C {
  public function m(named bool...): void {
    var_dump('m');
  }
  public static function s(int $a, named string...): void {
    var_dump($a);
  }
}

interface NamedContract {
  public function accepts(named int $value): void;
}

final class AbsorbingImplementation implements NamedContract {
  public function accepts(named mixed...): void {
    var_dump('absorbing implementation');
  }
}

function try_call((function(): void) $f): void {
  try {
    $f();
  } catch (Exception $e) {
    var_dump($e->getMessage());
  }
}

<<__EntryPoint>>
function main(): void {
  only_variadic(1);
  only_variadic(1, k=2);
  // Names sort before, between and after nothing in particular; all absorbed.
  only_variadic(1, zz='z', aa='a', mm='m');
  // Side effects of an absorbed argument still happen.
  only_variadic(1, k=(() ==> { var_dump('evaluated'); return 2; })());

  // Absorbed names sorting before, between and after the declared ones.
  mixed_named(1, b=2);
  mixed_named(1, b=2, d=4);
  mixed_named(1, a='a', b=2, c='c', d=4, e='e');
  mixed_named(1, zzz='z', b=2);
  mixed_named(1, aaa='a', b=2);

  // A required named param is still required, even though names are absorbed.
  try_call(() ==> mixed_named(1, d=4));
  try_call(() ==> mixed_named(1, nope='x'));

  with_positional_variadic(1, 2, 3);
  with_positional_variadic(1, 2, 3, a='a');
  with_positional_variadic(1, 2, 3, a='a', b='b');

  $c = new C();
  $c->m(flag=true);
  C::s(7, label='hi');

  // A named variadic accepts the explicit names in an overridden declaration.
  (new AbsorbingImplementation())->accepts(value=42);

  // The absorbing function reached through a dynamic call.
  $f = only_variadic<>;
  $f(1, k=2);

  // A function without a `named t...` parameter still rejects the argument.
  try_call(() ==> only_positional_variadic(1, k=2));
}
