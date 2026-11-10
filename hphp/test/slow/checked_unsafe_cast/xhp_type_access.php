<?hh

interface IHasData {
  abstract const type TData;
}

final class :my:host implements IHasData {
  const type TData = shape('a' => int);
}

abstract class Traverser {
  abstract const type THost as IHasData;
}

final class HostTraverser extends Traverser {
  const type THost = :my:host;
}

final class User {
  const type TTraverser = HostTraverser;

  public function cast(mixed $m): mixed {
    return HH\FIXME\UNSAFE_CAST<mixed, this::TTraverser::THost::TData>($m);
  }
}

<<__EntryPoint>>
function main(): void {
  $u = new User();
  var_dump($u->cast(shape('a' => 1)));
  var_dump($u->cast('a'));
}
