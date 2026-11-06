<?hh

class C {
  public arraykey $i = 0;
}

function takes_int(int $_): void {}

function assign_both(C $a, C $b): void {
  list($a->i, $b->i) = tuple('str', 1);
  takes_int($b->i);
}

<<__EntryPoint>>
function main(): void {
  $c = new C();
  assign_both($c, $c);
}
