<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

use namespace HH\Lib\{Keyset, Str};
use function HH\__Private\MiniTest\expect;
use type HH\__Private\MiniTest\{DataProvider, HackTest};

final class KeysetExtendedSuiteTest extends HackTest {

  public static function provideTestMap(): varray<mixed> {
    return vec[
      tuple(
        keyset['apple', 'banana', 'cherry'],
        $word ==> Str\uppercase($word),
        keyset['APPLE', 'BANANA', 'CHERRY'],
      ),
      tuple(
        keyset[1, 2, 3, 4],
        $x ==> $x * 2,
        keyset[2, 4, 6, 8],
      ),
      tuple(
        keyset[],
        $x ==> $x,
        keyset[],
      ),
    ];
  }

  <<DataProvider('provideTestMap')>>
  public function testMap<Tv as arraykey, Tr as arraykey>(
    Container<Tv> $input,
    (function(Tv): Tr) $func,
    keyset<Tr> $expected,
  ): void {
    expect(Keyset\map($input, $func))->toEqual($expected);
  }

  public static function provideTestFilter(): varray<mixed> {
    return vec[
      tuple(
        keyset[1, 2, 3, 4, 5, 6],
        $x ==> $x % 2 === 0,
        keyset[2, 4, 6],
      ),
      tuple(
        keyset['a', 'bb', 'ccc'],
        $s ==> Str\length($s) > 1,
        keyset['bb', 'ccc'],
      ),
      tuple(
        keyset[],
        $x ==> true,
        keyset[],
      ),
    ];
  }

  <<DataProvider('provideTestFilter')>>
  public function testFilter<Tv as arraykey>(
    Container<Tv> $input,
    (function(Tv): bool) $func,
    keyset<Tv> $expected,
  ): void {
    expect(Keyset\filter($input, $func))->toEqual($expected);
  }

  public static function provideTestUnion(): varray<mixed> {
    return vec[
      tuple(
        vec[
          keyset[1, 2, 3],
          keyset[3, 4, 5],
          keyset[5, 6, 7],
        ],
        keyset[1, 2, 3, 4, 5, 6, 7],
      ),
      tuple(
        vec[keyset[], keyset[]],
        keyset[],
      ),
    ];
  }

  <<DataProvider('provideTestUnion')>>
  public function testUnion<Tv as arraykey>(
    Container<Container<Tv>> $inputs,
    keyset<Tv> $expected,
  ): void {
    expect(Keyset\union(...$inputs))->toEqual($expected);
  }

  public static function provideTestIntersect(): varray<mixed> {
    return vec[
      tuple(
        keyset[1, 2, 3, 4, 5],
        vec[
          keyset[2, 3, 4, 6],
          keyset[3, 4, 5, 7],
        ],
        keyset[3, 4],
      ),
      tuple(
        keyset[1, 2],
        vec[keyset[3, 4]],
        keyset[],
      ),
    ];
  }

  <<DataProvider('provideTestIntersect')>>
  public function testIntersect<Tv as arraykey>(
    Container<Tv> $first,
    Container<Container<Tv>> $rest,
    keyset<Tv> $expected,
  ): void {
    expect(Keyset\intersect($first, ...$rest))->toEqual($expected);
  }

  public static function provideTestDiff(): varray<mixed> {
    return vec[
      tuple(
        keyset[1, 2, 3, 4, 5],
        vec[
          keyset[2, 3],
          keyset[4],
        ],
        keyset[1, 5],
      ),
      tuple(
        keyset[1, 2],
        vec[keyset[1, 2]],
        keyset[],
      ),
    ];
  }

  <<DataProvider('provideTestDiff')>>
  public function testDiff<Tv as arraykey>(
    Container<Tv> $first,
    Container<Container<Tv>> $rest,
    keyset<Tv> $expected,
  ): void {
    expect(Keyset\diff($first, ...$rest))->toEqual($expected);
  }
}
