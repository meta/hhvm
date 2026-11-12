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

// `$x is Item` shows that Item <: <expr>::TEnt, but the checker also concludes
// that <expr>::TEnt = Item, here and for every other receiver's TEnt.
function same_receiver(Ent $e): void {
  $x = $e->gen();
  if ($x is Item) {
    $e->gen()?->itemOnly();
  }
}

function other_receiver(Ent $e, Ent $f): void {
  $x = $e->gen();
  $y = $f->gen();
  if ($x is Item) {
    $y?->itemOnly();
  }
}

<<__EntryPoint>>
function main(): void {
  same_receiver(new AnyEnt());
}
