<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

use namespace HH\Lib\Math;
use function HH\__Private\MiniTest\expect;
use type HH\__Private\MiniTest\{DataProvider, HackTest};

final class MathCompareTest extends HackTest {

  public static function provideTestMaxva(): varray<mixed> {
    return vec[
      tuple(1, 2, vec[], 2),
      tuple(2, 1, vec[], 2),
      tuple(1.0, 2.0, vec[], 2.0),
      tuple(2.0, 1.0, vec[], 2.0),
      tuple(1, 1, vec[], 1),
      tuple(-2, -1, vec[], -1),
      tuple(1.0, 2, vec[], 2),
      tuple(1, 2.0, vec[], 2.0),
      tuple(-1, 1, vec[2, 3, 4, 5], 5),
      tuple(-1, 5, vec[4, 3, 2, 1], 5),
      tuple(Math\INT64_MIN, 0, vec[Math\INT64_MAX], Math\INT64_MAX),
      tuple(-10.5, -20.5, vec[-5.5, -30.0], -5.5),
    ];
  }

  <<DataProvider('provideTestMaxva')>>
  public function testMaxva<T as num>(
    T $first,
    T $second,
    Container<T> $rest,
    T $expected,
  ): void {
    expect(Math\maxva($first, $second, ...$rest))->toEqual($expected);
  }

  public static function provideTestMinva(): varray<mixed> {
    return vec[
      tuple(1, 2, vec[], 1),
      tuple(2, 1, vec[], 1),
      tuple(1.0, 2.0, vec[], 1.0),
      tuple(2.0, 1.0, vec[], 1.0),
      tuple(1, 1, vec[], 1),
      tuple(-2, -1, vec[], -2),
      tuple(1.0, 2, vec[], 1.0),
      tuple(1, 2.0, vec[], 1),
      tuple(1, -1, vec[-2, -3, -4, -5], -5),
      tuple(1, -5, vec[-4, -3, -2, -1], -5),
      tuple(Math\INT64_MAX, 0, vec[Math\INT64_MIN], Math\INT64_MIN),
      tuple(-10.5, -20.5, vec[-5.5, -30.0], -30.0),
    ];
  }

  <<DataProvider('provideTestMinva')>>
  public function testMinva<T as num>(
    T $first,
    T $second,
    Container<T> $rest,
    T $expected,
  ): void {
    expect(Math\minva($first, $second, ...$rest))->toEqual($expected);
  }

  public static function provideTestIsNan(): vec<(num, bool)> {
    return vec[
      tuple(0, false),
      tuple(1, false),
      tuple(-1, false),
      tuple(0.0, false),
      tuple(0.1, false),
      tuple(-0.1, false),
      tuple(Math\NAN, true),
      tuple(Math\INF, false),
      tuple(Math\INT64_MAX, false),
    ];
  }

  <<DataProvider('provideTestIsNan')>>
  public function testIsNan(num $number, bool $is_nan): void {
    expect(Math\is_nan($number))->toEqual($is_nan);
  }

  public static function provideTestAlmostEquals(): vec<(num, num, ?num, bool)>{
    return vec[
      tuple(.001, .002, .01, true),
      tuple(.002, .001, .01, true),
      tuple(.001, .002, null, false),
      tuple(.002, .001, null, false),
      tuple(.001, .002, 0, false),
      tuple(.002, .001, 0, false),
      tuple(.001, .001, 0, false),
      tuple(.001, .001, null, true),
      tuple(.00000001, .00000002, null, false),
      tuple(.00000001, .000000001, null, true),
      tuple(.001, 1, null, false),
      tuple(.001, 1, 1, true),
      tuple(1, 2, 1, false),
      tuple(1, 2, null, false),
      tuple(2, 1, 2, true),
      tuple(-10.0, -10.000000001, null, true),
      tuple(-10.0, -10.1, 0.2, true),
    ];
  }

  <<DataProvider('provideTestAlmostEquals')>>
  public function testAlmostEquals(
    num $num_one,
    num $num_two,
    ?num $epsilon,
    bool $expected,
    ): void{
    $epsilon is null
    ? expect(Math\almost_equals($num_one, $num_two))->toEqual($expected,)
    : expect(Math\almost_equals($num_one, $num_two, $epsilon))->toEqual(
      $expected
    );
  }
}
