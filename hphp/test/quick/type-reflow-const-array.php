<?hh

function loop_phi_join(int $iterations): int {
  $extra = vec[1];
  $scratch = vec[2];
  for ($round = 0; $round < $iterations; $round++) {
    $scratch = $extra;
    $extra = vec[3];
  }
  return $scratch[0] << 1;
}

<<__EntryPoint>>
function main(): void {
  var_dump(loop_phi_join(1));
}
