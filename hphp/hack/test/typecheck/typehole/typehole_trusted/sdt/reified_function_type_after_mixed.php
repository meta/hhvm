<?hh

function cast<<<__Enforceable>> reify T>(mixed $x): T {
  return $x as T;
}

// Under SDT both `mixed` and the function type are `supportdyn<...>`
<<__EntryPoint>>
function main(): void {
  cast<(mixed, (function(): int))>(tuple(1, 'not a function'));
}
