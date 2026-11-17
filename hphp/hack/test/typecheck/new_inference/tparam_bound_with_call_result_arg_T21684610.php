<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

interface I1<Ta> {}
interface I2<Tb> {}
final class A3 implements I2<float> {}

abstract class A1 {
  final public static function do<T, Ti as I1<T>>(
    I2<T> $_,
    Ti $_,
  ): this {
    invariant_violation('_');
  }
}

final class A2 extends A1 {
  private static function query(): A3 {
    invariant_violation('_');
  }

  final public static function call(): void {
    A2::do(self::query(), 1); // this passes!!!
  }

  final public static function call2(A3 $a): void {
    A2::do($a, 1); // this fails (int is not compatible with I1)
  }
}
