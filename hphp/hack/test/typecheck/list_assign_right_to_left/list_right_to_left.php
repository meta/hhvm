<?hh

class C {
  public arraykey $i = 0;
}

function takes_int(int $_): void {}

function inout_local(inout int $a): void {
  list($a, $a) = tuple('str', 1);
}

function property(C $c): void {
  list($c->i, $c->i) = tuple('str', 1);
  takes_int($c->i);
}

function aliased_properties(C $a, C $b): void {
  list($a->i, $b->i) = tuple('str', 1);
  takes_int($b->i);
}

function nested(C $c): void {
  list(list($c->i, $_x), $c->i) = tuple(tuple('str', 2), 1);
  takes_int($c->i);
}

function in_foreach(vec<(string, int)> $v): void {
  foreach ($v as list($a, $a)) {
    takes_int($a);
  }
}
