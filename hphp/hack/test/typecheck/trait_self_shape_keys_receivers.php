<?hh

trait Receivers {
  <<__ShapeKey>>
  const string KEY = 'key';

  public function union<
    T1 as shape(Receivers::KEY => int),
    T2 as shape(Receivers::KEY => int),
  >(bool $b, T1 $s1, T2 $s2): int {
    $s = $b ? $s1 : $s2;
    return $s[self::KEY];
  }

  public function intersection<
    T as shape(nameof Receivers => int) as shape('other' => string, ...),
  >(T $s): int {
    return $s[nameof self];
  }

  public function shape_or_dict<
    T1 as shape(Receivers::KEY => int),
    T2 as dict<string, int>,
  >(bool $b, T1 $s, T2 $d): mixed {
    $value = $b ? $s : $d;
    return $value[self::KEY];
  }
}
