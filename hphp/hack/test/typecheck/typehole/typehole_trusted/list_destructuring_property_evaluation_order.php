<?hh

class C {
  public arraykey $i = 0;
}

function takes_int(int $_): void {}

function assign_twice(C $c): void {
  list($c->i, $c->i) = tuple('str', 1);
  takes_int($c->i);
}

<<__EntryPoint>>
function main(): void {
  assign_twice(new C());
}
