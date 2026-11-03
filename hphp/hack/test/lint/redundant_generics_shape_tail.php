<?hh

function typed_tail_identity<T>(shape(T...) $x): shape(T...) {
  return $x;
}

function typed_tail_only_input<T>(shape(T...) $_x): void {}
