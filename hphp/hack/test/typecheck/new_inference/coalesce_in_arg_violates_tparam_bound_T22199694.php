<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

abstract final class A1 {
  public static function max<T as num>(T $first_number): T {
    return $first_number;
  }
  public static function last<Tv>(Traversable<Tv> $traversable): ?Tv {
    return null;
  }
  public static function funny(varray<int> $v): void {
    self::max(vec[self::last($v) ?? 0]);
  }
}
