<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

use namespace HH\Lib\{C, Dict, Keyset, Vec};
use function HH\__Private\MiniTest\expect;
use type HH\__Private\MiniTest\{DataProvider, HackTest};

final class CExtendedSuiteTest extends HackTest {

  public static function provideTestIsEmpty(): varray<mixed> {
    return vec[
      tuple(vec[], true),
      tuple(dict[], true),
      tuple(keyset[], true),
      tuple(vec[0], false),
      tuple(dict['k' => 'v'], false),
      tuple(keyset['item'], false),
      tuple(Map {}, true),
      tuple(Set {1, 2}, false),
    ];
  }

  <<DataProvider('provideTestIsEmpty')>>
  public function testIsEmpty<T>(
    Container<T> $container,
    bool $expected,
  ): void {
    expect(C\is_empty($container))->toEqual($expected);
  }

  public static function provideTestCount(): varray<mixed> {
    return vec[
      tuple(vec[], 0),
      tuple(vec[1, 2, 3], 3),
      tuple(dict['a' => 1, 'b' => 2], 2),
      tuple(keyset['x', 'y', 'z', 'w'], 4),
      tuple(Vector {10, 20, 30}, 3),
    ];
  }

  <<DataProvider('provideTestCount')>>
  public function testCount<T>(
    Container<T> $container,
    int $expected,
  ): void {
    expect(C\count($container))->toEqual($expected);
  }

  public static function provideTestContains(): varray<mixed> {
    return vec[
      tuple(vec[1, 2, 3], 2, true),
      tuple(vec[1, 2, 3], 4, false),
      tuple(dict['a' => 'alpha', 'b' => 'beta'], 'beta', true),
      tuple(dict['a' => 'alpha', 'b' => 'beta'], 'gamma', false),
      tuple(keyset['apple', 'banana'], 'banana', true),
      tuple(keyset['apple', 'banana'], 'cherry', false),
    ];
  }

  <<DataProvider('provideTestContains')>>
  public function testContains<T>(
    Container<T> $container,
    T $value,
    bool $expected,
  ): void {
    expect(C\contains($container, $value))->toEqual($expected);
  }

  public static function provideTestContainsKey(): varray<mixed> {
    return vec[
      tuple(dict['a' => 1, 'b' => 2], 'a', true),
      tuple(dict['a' => 1, 'b' => 2], 'c', false),
      tuple(vec[10, 20, 30], 1, true),
      tuple(vec[10, 20, 30], 5, false),
      tuple(keyset['foo', 'bar'], 'foo', true),
      tuple(keyset['foo', 'bar'], 'baz', false),
    ];
  }

  <<DataProvider('provideTestContainsKey')>>
  public function testContainsKey<Tk as arraykey, Tv>(
    KeyedContainer<Tk, Tv> $container,
    Tk $key,
    bool $expected,
  ): void {
    expect(C\contains_key($container, $key))->toEqual($expected);
  }

  public static function provideTestFind(): varray<mixed> {
    return vec[
      tuple(
        vec[1, 3, 5, 6, 7],
        $x ==> $x % 2 === 0,
        6,
      ),
      tuple(
        vec[1, 3, 5, 7],
        $x ==> $x % 2 === 0,
        null,
      ),
      tuple(
        dict['first' => 'apple', 'second' => 'avocado', 'third' => 'banana'],
        $x ==> $x[0] === 'b',
        'banana',
      ),
    ];
  }

  <<DataProvider('provideTestFind')>>
  public function testFind<T>(
    Traversable<T> $traversable,
    (function(T): bool) $predicate,
    ?T $expected,
  ): void {
    expect(C\find($traversable, $predicate))->toEqual($expected);
  }

  public static function provideTestIsSorted(): varray<mixed> {
    return vec[
      tuple(vec[], true),
      tuple(vec[1], true),
      tuple(vec[1, 2, 3, 4, 5], true),
      tuple(vec[1, 3, 2, 4], false),
      tuple(vec[5, 4, 3, 2, 1], false),
      tuple(vec['a', 'b', 'c'], true),
      tuple(vec['z', 'a'], false),
    ];
  }

  <<DataProvider('provideTestIsSorted')>>
  public function testIsSorted<T>(
    Traversable<T> $traversable,
    bool $expected,
  ): void {
    expect(C\is_sorted($traversable))->toEqual($expected);
  }
}
