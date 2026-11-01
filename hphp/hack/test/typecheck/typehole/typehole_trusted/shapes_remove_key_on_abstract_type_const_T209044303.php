<?hh

abstract class AC {
  abstract const type TC as shape('a' => int);

  public abstract function get(): this::TC;
}

class C extends AC {
  const type TC = shape('a' => int);

  public function get(): this::TC {
    return shape('a' => 42);
  }
}

function upcast(C $c): AC {
  return $c;
}

<<__EntryPoint>>
function main(): void {
  $c = new C();
  $ac = upcast($c);
  $s = $ac->get();
  Shapes::removeKey(inout $s, 'a');
  takes_tc($s);
}

function takes_tc(shape('a' => int) $s): void {
  $s['a'];
}
