<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

interface NamedContract {
  public function describe(named int $z): string;
}

final class NamedImplementation implements NamedContract {
  public function describe(
    named bool $a = false,
    named int $z,
    named string $zz = '',
  ): string {
    return 'named: '.$z.' '.($a ? 'true' : 'false');
  }
}

abstract class PositionalContract {
  abstract public function describe(bool $p): string;
}

final class PositionalImplementation extends PositionalContract {
  public function describe(named bool $a = false, bool $p): string {
    return 'positional: '.($p ? 'true' : 'false').' '.($a ? 'true' : 'false');
  }
}

class InoutBase {
  public function increment(inout int $value): void {
    $value++;
  }
}

final class InoutImplementation extends InoutBase {
  public function increment(
    // TODO(T294006289): restore when inout and named are allowed in the same definition
    // named bool $unused = false,
    inout int $value,
  ): void {
    $value++;
  }
}

<<__EntryPoint>>
function main(): void {
  $named = new NamedImplementation();
  $positional = new PositionalImplementation();
  $inout = new InoutImplementation();

  var_dump($named->describe(z=42));
  var_dump($positional->describe(true));
  var_dump(get_class($inout));
}
