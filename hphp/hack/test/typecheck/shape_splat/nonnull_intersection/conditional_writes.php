<?hh
<<file:__EnableUnstableFeatures('shape_splat_expression')>>

type NonnullSplatCard = shape(
  'f0' => int,
  'f1' => int,
  'f2' => int,
  'f3' => int,
  'f4' => int,
  'f5' => int,
  'f6' => int,
  'f7' => int,
  'f8' => int,
  'f9' => int,
  'f10' => int,
  'f11' => int,
  'f12' => int,
  'f13' => int,
  'f14' => int,
  'f15' => int,
  'f16' => int,
  'f17' => int,
  'f18' => int,
  'f19' => int,
  ?'alt_title' => string,
  ?'alt_prompt' => string,
  ?'w0' => bool,
  ?'w1' => bool,
  ?'w2' => bool,
  ?'w3' => bool,
  ?'w4' => bool,
);

abstract final class NonnullSplatConditionalWrites {
  public static function build(vec<int> $xs): vec<NonnullSplatCard> {
    $out = vec[];
    $n = 0;
    foreach ($xs as $x) {
      if ($x === 7) {
        continue;
      }
      $alt = self::altFor($x);
      $v = self::valFor($x);
      $out[] = shape(
        ...($alt ?? shape()),
        'f0' => $v,
        'f1' => $v,
        'f2' => $v,
        'f3' => $v,
        'f4' => $v,
        'f5' => $v,
        'f6' => $v,
        'f7' => $v,
        'f8' => $v,
        'f9' => $v,
        'f10' => $v,
        'f11' => $v,
        'f12' => $v,
        'f13' => $v,
        'f14' => $v,
        'f15' => $v,
        'f16' => $v,
        'f17' => $v,
        'f18' => $v,
        'f19' => $v,
      );
      $n++;
      if ($x > 0) {
        $out[$n - 1]['w0'] = true;
      }
      if ($x > 1) {
        $out[$n - 1]['w1'] = true;
      }
      if ($x > 2) {
        $out[$n - 1]['w2'] = true;
      }
      if ($x > 3) {
        $out[$n - 1]['w3'] = true;
      }
      if ($x > 4) {
        $out[$n - 1]['w4'] = true;
      }
    }
    return $out;
  }

  public static function altFor(
    int $x,
  ): ?shape('alt_title' => string, 'alt_prompt' => string) {
    return $x > 3 ? shape('alt_title' => 'a', 'alt_prompt' => 'b') : null;
  }

  public static function valFor(int $x): int {
    return $x + 1;
  }
}
