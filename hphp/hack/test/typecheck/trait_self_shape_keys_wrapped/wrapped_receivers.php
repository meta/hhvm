<?hh
<<file:__EnableUnstableFeatures('like_type_hints')>>

trait T {
  <<__ShapeKey>>
  const string KEY = 'key';

  public function like(~shape(T::KEY => int) $s): mixed {
    return $s[self::KEY];
  }

  public function supportdyn(supportdyn<shape(nameof T => int)> $s): mixed {
    return $s[nameof self];
  }

  public function nullable(?shape(T::KEY => int) $s): ?int {
    return $s[self::KEY] ?? null;
  }

  public function bound<
    TS as supportdyn<~?shape(nameof T => int)>,
  >(TS $s): mixed {
    return $s[nameof self] ?? null;
  }
}
