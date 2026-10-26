<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

// A `named t...` parameter alongside a positional variadic. Calls with more
// positional arguments than declared take the packing prologue, so the argument
// region has a packed vec on top of the absorbed names.
function both_variadics(
  int $x,
  named int $b = 20,
  int ...$args,
  named arraykey...
): void {
  var_dump(vec[$x, $b], $args);
}

// A `named t...` parameter alongside reified generics, which sit on top of the
// argument region.
function reified<reify T>(int $x, named arraykey...): void {
  var_dump($x);
}

class C {
  public function __construct(public int $x, named arraykey...) {}
}

class D {
  <<__Memoize>>
  public function m(int $x, named arraykey...): int {
    return $x * 2;
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
  both_variadics(1);
  both_variadics(1, b=2);
  both_variadics(1, 2, 3);
  both_variadics(1, 2, 3, b=4);
  both_variadics(1, 2, 3, zz='z', b=4, aa='a');
  both_variadics(1, zz='z');
  $v = vec[1];
  both_variadics(...$v, zz='z', b=4, aa='a');
  $v = vec[1, 2];
  both_variadics(...$v, zz='z', b=4, aa='a');

  reified<int>(1);
  reified<int>(1, k=2);
  reified<int>(1, zz='z', aa='a');

  $c = new C(1, k=2);
  var_dump($c->x);

  // <<__Memoize>> on a free function with any variadic is rejected by hackc, so
  // only the method case is reachable.
  $d = new D();
  var_dump($d->m(5, junk='x'));

  // Closures and lambdas.
  $f = (int $x, named arraykey...) ==> $x * 10;
  var_dump($f(3, k=2));
  $g = function(int $x, named arraykey...): int { return $x * 100; };
  var_dump($g(3, k=2));

  // Absorbing works through a variadic-arg unpack too.
  $v = vec[7, 8];
  both_variadics(1, ...$v, zz='z');

  // The absorbed name is not a positional argument, so arity is unaffected.
  try_call(() ==> reified<int>());
}
