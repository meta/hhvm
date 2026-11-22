<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

use namespace HH\Lib\{C, Vec};
use function HH\__Private\MiniTest\expect;
use type HH\__Private\MiniTest\{DataProvider, HackTest};

final class VecExtendedSuiteTest extends HackTest {

  public static function provideConcatCases(): vec<(Traversable<int>, Traversable<int>, vec<int>)> {
    return vec[
      tuple(vec[1, 2], vec[3, 4], vec[1, 2, 3, 4]),
      tuple(vec[], vec[1, 2], vec[1, 2]),
      tuple(vec[1, 2], vec[], vec[1, 2]),
      tuple(vec[], vec[], vec[]),
    ];
  }

  <<DataProvider('provideConcatCases')>>
  public function testConcat(
    Traversable<int> $first,
    Traversable<int> $second,
    vec<int> $expected,
  ): void {
    expect(Vec\concat($first, $second))->toEqual($expected);
  }

  public static function provideFillCases(): vec<(int, int, vec<int>)> {
    return vec[
      tuple(5, 42, vec[42, 42, 42, 42, 42]),
      tuple(1, 0, vec[0]),
      tuple(0, 7, vec[]),
    ];
  }

  <<DataProvider('provideFillCases')>>
  public function testFill(int $size, int $value, vec<int> $expected): void {
    expect(Vec\fill($size, $value))->toEqual($expected);
  }

  public function testFillNegativeSize(): void {
    expect(() ==> Vec\fill(-1, 5))->toThrow(InvariantException::class);
  }

  public static function provideFilterCases(): vec<(vec<int>, (function(int): bool), vec<int>)> {
    return vec[
      tuple(vec[1, 2, 3, 4, 5, 6], $x ==> $x % 2 === 0, vec[2, 4, 6]),
      tuple(vec[1, 3, 5], $x ==> $x % 2 === 0, vec[]),
      tuple(vec[2, 4, 6], $x ==> $x % 2 === 0, vec[2, 4, 6]),
      tuple(vec[], $x ==> true, vec[]),
    ];
  }

  <<DataProvider('provideFilterCases')>>
  public function testFilter(
    vec<int> $input,
    (function(int): bool) $predicate,
    vec<int> $expected,
  ): void {
    expect(Vec\filter($input, $predicate))->toEqual($expected);
  }

  public static function provideMapCases(): vec<(vec<int>, (function(int): int), vec<int>)> {
    return vec[
      tuple(vec[1, 2, 3], $x ==> $x * 2, vec[2, 4, 6]),
      tuple(vec[0, -1, 5], $x ==> $x + 10, vec[10, 9, 15]),
      tuple(vec[], $x ==> $x, vec[]),
    ];
  }

  <<DataProvider('provideMapCases')>>
  public function testMap(
    vec<int> $input,
    (function(int): int) $fn,
    vec<int> $expected,
  ): void {
    expect(Vec\map($input, $fn))->toEqual($expected);
  }

  public static function provideReverseCases(): vec<(vec<mixed>, vec<mixed>)> {
    return vec[
      tuple(vec[1, 2, 3, 4], vec[4, 3, 2, 1]),
      tuple(vec['a', 'b', 'c'], vec['c', 'b', 'a']),
      tuple(vec[42], vec[42]),
      tuple(vec[], vec[]),
    ];
  }

  <<DataProvider('provideReverseCases')>>
  public function testReverse(vec<mixed> $input, vec<mixed> $expected): void {
    expect(Vec\reverse($input))->toEqual($expected);
  }

  public static function provideSortCases(): vec<(vec<int>, vec<int>)> {
    return vec[
      tuple(vec[5, 2, 8, 1, 9], vec[1, 2, 5, 8, 9]),
      tuple(vec[-3, 0, -10, 5], vec[-10, -3, 0, 5]),
      tuple(vec[1], vec[1]),
      tuple(vec[], vec[]),
    ];
  }

  <<DataProvider('provideSortCases')>>
  public function testSort(vec<int> $input, vec<int> $expected): void {
    expect(Vec\sort($input))->toEqual($expected);
  }

  public static function provideZipCases(): vec<(vec<int>, vec<string>, vec<(int, string)>)> {
    return vec[
      tuple(
        vec[1, 2, 3],
        vec['a', 'b', 'c'],
        vec[tuple(1, 'a'), tuple(2, 'b'), tuple(3, 'c')],
      ),
      tuple(
        vec[1, 2],
        vec['a', 'b', 'c'],
        vec[tuple(1, 'a'), tuple(2, 'b')],
      ),
      tuple(
        vec[1, 2, 3],
        vec['a'],
        vec[tuple(1, 'a')],
      ),
      tuple(vec[], vec['a', 'b'], vec[]),
    ];
  }

  <<DataProvider('provideZipCases')>>
  public function testZip(
    vec<int> $first,
    vec<string> $second,
    vec<(int, string)> $expected,
  ): void {
    expect(Vec\zip($first, $second))->toEqual($expected);
  }

  public function testRangeAndCount(): void {
    expect(Vec\range(1, 5))->toEqual(vec[1, 2, 3, 4, 5]);
    expect(Vec\range(5, 1))->toEqual(vec[5, 4, 3, 2, 1]);
    expect(Vec\range(1, 1))->toEqual(vec[1]);
    expect(C\count(Vec\range(1, 100)))->toEqual(100);
  }
}
