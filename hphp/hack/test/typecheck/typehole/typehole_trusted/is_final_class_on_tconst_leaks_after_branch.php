<?hh

abstract class Ent {
  abstract const type TEnt as Ent super this;

  abstract public function gen(): ?this::TEnt;
}

final class Item extends Ent {
  const type TEnt = Item;

  public function gen(): ?Item {
    return null;
  }

  public function itemOnly(): void {}
}

final class Other extends Ent {
  const type TEnt = Other;

  public function gen(): ?Other {
    return new Other();
  }
}

final class AnyEnt extends Ent {
  const type TEnt = Ent;

  private int $calls = 0;

  public function gen(): ?Ent {
    $this->calls++;
    return $this->calls === 1 ? new Item() : new Other();
  }
}

// The false fact <expr>::TEnt = Item from `$x is Item` must not reach values
// that leave the branch.
function value_from_before_branch(Ent $e): void {
  $x = $e->gen();
  $z = $e->gen();
  $w = null;
  if ($x is Item) {
    $w = $z;
  }
  $w?->itemOnly();
}

function value_from_branch(Ent $e): void {
  $x = $e->gen();
  $w = null;
  if ($x is Item) {
    $w = $e->gen();
  }
  $w?->itemOnly();
}

<<__EntryPoint>>
function main(): void {
  value_from_before_branch(new AnyEnt());
}
