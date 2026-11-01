<?hh

abstract class C {
  abstract const type T;
}

function id1<T as C with { type T = mixed }, Tacc>(mixed $x): Tacc
where
  T as C with { type T = Tacc } {
  return $x;
}

function id2<T as C with { type T = nothing }, Tacc>(Tacc $x): nothing
where
  T as C with { type T = Tacc } {
  return $x;
}

function id<T as C with { type T = nothing } as C with { type T = mixed }>(
  mixed $x,
): nothing {
  // Bad! T does not define the type constant ::T
  // unambiguously.
  return id2<T, _>(id1<T, _>($x));
}

function spoof(mixed $x): nothing {
  // Legit, nothing is a subtype of everything
  return id<nothing>($x);
}
