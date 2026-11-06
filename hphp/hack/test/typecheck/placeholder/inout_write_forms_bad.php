<?hh

function flat_list(inout int $_): void {
  list($_, $x) = tuple('str', 1);
}

function foreach_value(inout int $_): void {
  foreach (vec['str'] as $_) {}
}

function both_inout(inout int $_, inout string $_): void {
  $_ = 'str';
}

function second_inout_unchanged(inout string $_, inout int $_): void {
  $_ = 'str';
}

function in_one_branch(inout int $_, bool $b): void {
  if ($b) {
    $_ = 'str';
  }
}

function tuple_destructure(inout int $_): void {
  tuple($_, $y) = tuple('str', 1);
}

function shape_destructure(inout int $_): void {
  shape('a' => $_) = shape('a' => 'str');
}

function same_type(inout int $_): void {
  $_ = 42;
}
