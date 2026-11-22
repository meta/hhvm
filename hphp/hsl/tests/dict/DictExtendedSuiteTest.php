<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

use namespace HH\Lib\{Dict, Math, Str};
use function HH\__Private\MiniTest\expect;
use type HH\__Private\MiniTest\{DataProvider, HackTest};

final class DictExtendedSuiteTest extends HackTest {

  public static function provideTestMapWithKey(): varray<mixed> {
    return vec[
      tuple(
        dict['a' => 1, 'b' => 2, 'c' => 3],
        ($k, $v) ==> Str\format('%s:%d', $k, $v),
        dict['a' => 'a:1', 'b' => 'b:2', 'c' => 'c:3'],
      ),
      tuple(
        dict[],
        ($k, $v) ==> $v,
        dict[],
      ),
      tuple(
        dict[10 => 100, 20 => 200],
        ($k, $v) ==> $k + $v,
        dict[10 => 110, 20 => 220],
      ),
    ];
  }

  <<DataProvider('provideTestMapWithKey')>>
  public function testMapWithKey<Tk as arraykey, Tv, Tr>(
    KeyedContainer<Tk, Tv> $input,
    (function(Tk, Tv): Tr) $func,
    dict<Tk, Tr> $expected,
  ): void {
    expect(Dict\map_with_key($input, $func))->toEqual($expected);
  }

  public static function provideTestFilterWithKey(): varray<mixed> {
    return vec[
      tuple(
        dict['apple' => 5, 'banana' => 3, 'cherry' => 6],
        ($k, $v) ==> Str\length($k) > 5 && $v > 4,
        dict['cherry' => 6],
      ),
      tuple(
        dict[],
        ($k, $v) ==> true,
        dict[],
      ),
    ];
  }

  <<DataProvider('provideTestFilterWithKey')>>
  public function testFilterWithKey<Tk as arraykey, Tv>(
    KeyedContainer<Tk, Tv> $input,
    (function(Tk, Tv): bool) $func,
    dict<Tk, Tv> $expected,
  ): void {
    expect(Dict\filter_with_key($input, $func))->toEqual($expected);
  }

  public static function provideTestGroupBy(): varray<mixed> {
    return vec[
      tuple(
        vec['apple', 'apricot', 'banana', 'blueberry', 'cherry'],
        $word ==> $word[0],
        dict[
          'a' => vec['apple', 'apricot'],
          'b' => vec['banana', 'blueberry'],
          'c' => vec['cherry'],
        ],
      ),
      tuple(
        vec[],
        $x ==> $x,
        dict[],
      ),
    ];
  }

  <<DataProvider('provideTestGroupBy')>>
  public function testGroupBy<Tv, Tk as arraykey>(
    Container<Tv> $input,
    (function(Tv): Tk) $key_func,
    dict<Tk, vec<Tv>> $expected,
  ): void {
    expect(Dict\group_by($input, $key_func))->toEqual($expected);
  }

  public static function provideTestFromKeys(): varray<mixed> {
    return vec[
      tuple(
        vec['a', 'b', 'c'],
        $k ==> Str\uppercase($k),
        dict['a' => 'A', 'b' => 'B', 'c' => 'C'],
      ),
      tuple(
        vec[],
        $k ==> $k,
        dict[],
      ),
    ];
  }

  <<DataProvider('provideTestFromKeys')>>
  public function testFromKeys<Tk as arraykey, Tv>(
    Container<Tk> $keys,
    (function(Tk): Tv) $value_func,
    dict<Tk, Tv> $expected,
  ): void {
    expect(Dict\from_keys($keys, $value_func))->toEqual($expected);
  }

  public static function provideTestUniqueBy(): varray<mixed> {
    return vec[
      tuple(
        dict['a' => 1, 'b' => 2, 'c' => 1, 'd' => 3, 'e' => 2],
        $v ==> $v,
        dict['c' => 1, 'e' => 2, 'd' => 3],
      ),
      tuple(
        dict[],
        $v ==> $v,
        dict[],
      ),
    ];
  }

  <<DataProvider('provideTestUniqueBy')>>
  public function testUniqueBy<Tk as arraykey, Tv, Ts as arraykey>(
    KeyedContainer<Tk, Tv> $input,
    (function(Tv): Ts) $scalar_func,
    dict<Tk, Tv> $expected,
  ): void {
    expect(Dict\unique_by($input, $scalar_func))->toEqual($expected);
  }
}
