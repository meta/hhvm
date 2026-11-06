<?hh

class C {
  public arraykey $i = 0;
}

function takes_int(int $_): void {}

function takes_string(string $_): void {}

function distinct_targets(): void {
  list($a, $b) = tuple('str', 1);
  takes_string($a);
  takes_int($b);
}

function leftmost_written_last(C $c): void {
  list($c->i, $c->i) = tuple(1, 'str');
  takes_int($c->i);
}

function nested_written_last(C $c): void {
  list($c->i, list($c->i, $_x)) = tuple(1, tuple('str', 2));
  takes_int($c->i);
}
