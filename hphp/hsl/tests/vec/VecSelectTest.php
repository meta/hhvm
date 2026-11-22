<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

use namespace HH\Lib\{C, Math, Str, Vec};
use function HH\__Private\MiniTest\expect;
use type HH\__Private\MiniTest\{DataProvider, HackTest};

final class VecSelectTest extends HackTest {

  public static function provideTestDiff(): varray<mixed> {
    return vec[
      tuple(
        vec[],
        vec[
          dict[],
        ],
        vec[],
      ),
      tuple(
        new Vector(Vec\range(0, 20)),
        vec[
          Set {1, 3, 5},
          Map {'foo' => 7, 'bar' => 9},
          HackLibTestTraversables::getIterator(Vec\range(11, 30)),
        ],
        vec[0, 2, 4, 6, 8, 10],
      ),
    ];
  }

  <<DataProvider('provideTestDiff')>>
  public function testDiff<Tv as arraykey>(
    Traversable<Tv> $base,
    Container<Container<Tv>> $traversables,
    vec<Tv> $expected,
  ): void {
    expect(Vec\diff(
      $base,
      C\firstx($traversables),
      ...Vec\slice($traversables, 1)
    ))->toEqual($expected);
  }

  public static function provideTestDiffBy(): varray<mixed> {
    return vec[
      tuple(
        vec['the', 'quick', 'brown', 'fox'],
        vec[],
        $x ==> $x,
        vec['the', 'quick', 'brown', 'fox'],
      ),
      tuple(
        vec[],
        Vector {'the', 'quick', 'brown', 'fox'},
        $x ==> $x,
        vec[],
      ),
      tuple(
        Set {'plum', 'port', 'paste', 'pun', 'promise'},
        HackLibTestTraversables::getIterator(
          vec['power', 'push', 'pin', 'pygmy'],
        ),
        ($str) ==> Str\slice($str, 0, 2),
        vec['plum', 'paste', 'promise'],
      ),
    ];
  }

  <<DataProvider('provideTestDiffBy')>>
  public function testDiffBy<Tv, Ts as arraykey>(
    Traversable<Tv> $first,
    Traversable<Tv> $second,
    (function(Tv): Ts) $scalar_func,
    vec<Tv> $expected,
  ): void {
    expect(Vec\diff_by($first, $second, $scalar_func))
      ->toEqual($expected);
  }

  public static function provideDrop(): varray<mixed> {
    return vec[
      tuple(
        vec[],
        5,
        vec[],
      ),
      tuple(
        Vec\range(0, 5),
        0,
        vec[0, 1, 2, 3, 4, 5],
      ),
      tuple(
        new Vector(Vec\range(0, 5)),
        10,
        vec[],
      ),
      tuple(
        new Set(Vec\range(0, 5)),
        2,
        vec[2, 3, 4, 5],
      ),
      tuple(
        HackLibTestTraversables::getIterator(Vec\range(0, 5)),
        5,
        vec[5],
      ),
    ];
  }

  <<DataProvider('provideDrop')>>
  public function testDrop<Tv>(
    Traversable<Tv> $traversable,
    int $n,
    vec<Tv> $expected,
  ): void {
    expect(Vec\drop($traversable, $n))->toEqual($expected);
  }

  public static function provideTestFilter(): varray<mixed> {
    return vec[
      tuple(
        dict[],
        $x ==> true,
        vec[],
      ),
      tuple(
        dict[0 => 1],
        $x ==> true,
        vec[1],
      ),
      tuple(
        dict[0 => 1],
        $x ==> false,
        vec[],
      ),
      tuple(
        Vec\range(1, 10),
        $x ==> $x % 2 === 0,
        vec[2, 4, 6, 8, 10],
      ),
      tuple(
        Map {'donald' => 'duck', 'daffy' => 'duck', 'mickey' => 'mouse'},
        $x ==> $x === 'duck',
        vec['duck', 'duck'],
      ),
      tuple(
        HackLibTestTraversables::getIterator(Vec\range(0, 5)),
        $x ==> $x % 2 === 0,
        vec[0, 2, 4],
      ),
    ];
  }

  <<DataProvider('provideTestFilter')>>
  public function testFilter<Tv>(
    Traversable<Tv> $traversable,
    (function(Tv): bool) $value_predicate,
    vec<Tv> $expected,
  ): void {
    expect(Vec\filter($traversable, $value_predicate))->toEqual($expected);
  }

  public function testFilterWithoutPredicate(): void {
    expect(Vec\filter(vec[
      0, 3, null, 5, false, 40, '', '0', 'win!',
    ]))->toEqual(vec[3, 5, 40, 'win!']);
  }

  public static function provideTestFilterNulls(): varray<mixed> {
    return vec[
      tuple(
        vec[null, null, null],
        vec[],
      ),
      tuple(
        Map {
          'foo' => false,
          'bar' => null,
          'baz' => '',
          'qux' => 0,
        },
        vec[false, '', 0],
      ),
      tuple(
        Vector {
          'foo',
          'bar',
          null,
          'baz',
        },
        vec['foo', 'bar', 'baz'],
      ),
      tuple(
        HackLibTestTraversables::getKeyedIterator(dict[
          1 => null,
          2 => vec[],
          3 => '0',
        ]),
        vec[vec[], '0'],
      ),
    ];
  }

  <<DataProvider('provideTestFilterNulls')>>
  public function testFilterNulls<Tv>(
  Traversable<?Tv> $traversable,
  vec<Tv> $expected,
  ): void {
    expect(Vec\filter_nulls($traversable))->toEqual($expected);
  }

  public static function provideTestFilterWithKey(): darray<string, mixed> {
    return dict[
      'All elements selected' => tuple(
        vec['the', 'quick', 'brown', 'fox', 'jumped'],
        ($key, $value) ==> true,
        vec['the', 'quick', 'brown', 'fox', 'jumped'],
      ),
      'No elements selected' => tuple(
        vec['the', 'quick', 'brown', 'fox', 'jumped'],
        ($key, $value) ==> false,
        vec[],
      ),
      'odd elements selected' => tuple(
        vec['the', 'quick', 'brown', 'fox', 'jumped'],
        ($key, $value) ==> $key % 2 === 1,
        vec['quick','fox'],
      ),
      'elements selected starting with "f"' => tuple(
        vec['the', 'quick', 'brown', 'fox', 'jumped'],
        ($key, $value) ==> Str\starts_with($value, 'f'),
        vec['fox'],
      ),
      'elements selected starting with "f" 2' => tuple(
        HackLibTestTraversables::getIterator(
          vec['the', 'quick', 'brown', 'fox', 'jumped']
        ),
        ($key, $value) ==> Str\starts_with($value, 'f'),
        vec['fox'],
      ),
    ];
  }

  <<DataProvider('provideTestFilterWithKey')>>
  public function testFilterWithKey<Tk, Tv>(
    KeyedTraversable<Tk, Tv> $traversable,
    (function(Tk, Tv): bool) $filter_func,
    vec<Tv> $expected,
  ): void {
    $result = Vec\filter_with_key($traversable, $filter_func);
    expect($result)->toEqual($expected);
  }

  public static function provideTestIntersect(): varray<mixed> {
    return vec[
      tuple(
        Vec\range(0, 1000),
        vec[],
        vec[],
        vec[],
      ),
      tuple(
        Vec\range(1, 10),
        Vec\range(1, 5),
        vec[
          Vec\range(2, 6),
          Vec\range(3, 7),
        ],
        vec[3, 4, 5],
      ),
      tuple(
        Set {},
        Vec\range(1, 100),
        vec[],
        vec[],
      ),
      tuple(
        Vec\range(1, 1000),
        Map {},
        vec[
          Set {},
          Vector {},
        ],
        vec[],
      ),
      tuple(
        new Vector(Vec\range(1, 100)),
        Map {1 => 2, 39 => 40},
        vec[
          HackLibTestTraversables::getIterator(Vec\range(0, 40)),
        ],
        vec[2, 40],
      ),
      tuple(
        vec[3, 4, 4, 5],
        vec[3, 4],
        vec[],
        vec[3, 4, 4],
      ),
      tuple(
        HackLibTestTraversables::getGenerator(Vec\range(0, 4)),
        vec[3],
        vec[],
        vec[3],
      ),
    ];
  }

  <<DataProvider('provideTestIntersect')>>
  public function testIntersect<Tv as arraykey>(
    Traversable<Tv> $first,
    Traversable<Tv> $second,
    Container<Container<Tv>> $rest,
    vec<Tv> $expected,
  ): void {
    expect(Vec\intersect($first, $second, ...$rest))->toEqual($expected);
  }

  public static function provideTestKeys(): varray<mixed> {
    return vec[
      tuple(
        dict[
          'foo' => null,
          'bar' => null,
          'baz' => null,
        ],
        vec['foo', 'bar', 'baz'],
      ),
      tuple(
        Map {
          '' => null,
          '0' => null,
          0 => null,
          'foo' => null,
          'false' => null,
        },
        vec['', '0', 0, 'foo', 'false'],
      ),
      tuple(
        Vector {
          'foo',
          'bar',
          null,
          'baz',
        },
        vec[0, 1, 2, 3],
      ),
      tuple(
        HackLibTestTraversables::getKeyedIterator(dict[
          1 => null,
          2 => vec[],
          3 => '0',
        ]),
        vec[1, 2, 3],
      ),
    ];
  }

  <<DataProvider('provideTestKeys')>>
  public function testKeys<Tk, Tv>(
    KeyedTraversable<Tk, Tv> $traversable,
    vec<Tk> $expected,
  ): void {
    expect(Vec\keys($traversable))->toEqual($expected);
  }

  public static function provideTestSample(): varray<mixed> {
    return vec[
      tuple(
        Vec\range(0, 5),
        6,
      ),
      tuple(
        Vec\range(0, 5),
        1,
      ),
      tuple(
        Vec\range(0, 5),
        10,
      ),
    ];
  }

  <<DataProvider('provideTestSample')>>
  public function testSample<Tv>(
    Traversable<Tv> $traversable,
    int $sample_size,
  ): void {
    $expected_size = Math\minva(C\count(vec($traversable)), $sample_size);
    expect(C\count(Vec\sample($traversable, $sample_size)))
      ->toEqual($expected_size);
  }

  public function testSampleIterator(): void {
    $iterator = HackLibTestTraversables::getIterator(Vec\range(0, 5));
    expect(C\count(Vec\sample($iterator, 3)))->toEqual(3);
  }

  public static function provideTestSlice(): varray<mixed> {
    return vec[
      tuple(
        Vec\range(0, 5),
        6,
        1,
        vec[],
      ),
      tuple(
        new Vector(Vec\range(0, 5)),
        2,
        10,
        Vec\range(2, 5),
      ),
      tuple(
        new Set(Vec\range(0, 5)),
        2,
        0,
        vec[],
      ),
      tuple(
        new Vector(Vec\range(0, 5)),
        2,
        null,
        Vec\range(2, 5),
      ),
      tuple(
        Vec\range(0, 5),
        -6,
        0,
        vec[],
      ),
      tuple(
        new Vector(Vec\range(0, 5)),
        -2,
        null,
        Vec\range(4, 5),
      ),
      tuple(
        new Set(Vec\range(0, 5)),
        -4,
        3,
        Vec\range(2, 4),
      ),
      tuple(
        dict['foo' => 1, 'bar' => 2, 'foobar' => 3],
        -2,
        null,
        Vec\range(2, 3),
      ),
      tuple(
        new Set(Vec\range(0, 5)),
        8,
        null,
        vec[],
      ),
    ];
  }

  <<DataProvider('provideTestSlice')>>
  public function testSlice<Tv>(
    Container<Tv> $container,
    int $offset,
    ?int $length,
    vec<Tv> $expected,
  ): void {
    expect(Vec\slice($container, $offset, $length))->toEqual($expected);
  }

  public function testSliceThrow<Tv>(): void {
    expect(() ==> Vec\slice(Vec\range(0, 5), -7, 1))->toThrow(
      InvariantException::class,
    );
    expect(() ==> Vec\slice(Vec\range(0, 5), 10, null))->notToThrow();
  }

  public static function provideTake(): varray<mixed> {
    return vec[
      tuple(
        vec[],
        5,
        vec[],
      ),
      tuple(
        Vec\range(0, 5),
        0,
        vec[],
      ),
      tuple(
        new Vector(Vec\range(0, 5)),
        10,
        vec[0, 1, 2, 3, 4, 5],
      ),
      tuple(
        new Set(Vec\range(0, 5)),
        2,
        vec[0, 1],
      ),
      tuple(
        HackLibTestTraversables::getIterator(Vec\range(0, 5)),
        5,
        vec[0, 1, 2, 3, 4],
      ),
    ];
  }

  <<DataProvider('provideTake')>>
  public function testTake<Tv>(
    Traversable<Tv> $traversable,
    int $n,
    vec<Tv> $expected,
  ): void {
    expect(Vec\take($traversable, $n))->toEqual($expected);
  }

  public function testTakeIter(): void {
    $iter = HackLibTestTraversables::getIterator(Vec\range(0, 4));
    expect(Vec\take($iter, 2))->toEqual(vec[0, 1]);
    expect(Vec\take($iter, 0))->toEqual(vec[]);
    expect(Vec\take($iter, 2))->toEqual(vec[2, 3]);
    expect(Vec\take($iter, 2))->toEqual(vec[4]);
  }

  public static function provideTestUnique(): varray<mixed> {
    return vec[
      tuple(
        vec['the', 'quick', 'brown', 'fox', 'jumped', 'over', 'the', 'dog'],
        vec['the', 'quick', 'brown', 'fox', 'jumped', 'over', 'dog'],
      ),
      tuple(
        HackLibTestTraversables::getIterator(
          vec[1, 2, 3, 2, 3, 4, 5, 6],
        ),
        vec[1, 2, 3, 4, 5, 6],
      ),
    ];
  }

  <<DataProvider('provideTestUnique')>>
  public function testUnique<Tv as arraykey>(
    Traversable<Tv> $traversable,
    vec<Tv> $expected,
  ): void {
    expect(Vec\unique($traversable))->toEqual($expected);
  }

  public static function provideTestUniqueBy(): varray<mixed> {
    return vec[
      tuple(
        vec[
          'plum',
          'port',
          'power',
          'push',
          'pin',
          'pygmy',
          'paste',
          'plate',
        ],
        ($str) ==> Str\slice($str, 0, 2),
        vec['plate', 'power', 'push', 'pin', 'pygmy', 'paste'],
      ),
      tuple(
        HackLibTestTraversables::getIterator(
          vec[
            'plum',
            'port',
            'power',
            'push',
            'pin',
            'pygmy',
            'paste',
            'plate',
          ],
        ),
        ($str) ==> Str\slice($str, 0, 2),
        vec['plate', 'power', 'push', 'pin', 'pygmy', 'paste'],
      ),
    ];
  }

  <<DataProvider('provideTestUniqueBy')>>
  public function testUniqueBy<Tv, Ts as arraykey>(
    Traversable<Tv> $traversable,
    (function(Tv): Ts) $scalar_func,
    vec<Tv> $expected,
  ): void {
    expect(Vec\unique_by($traversable, $scalar_func))
      ->toEqual($expected);
  }

  public function testSliceEdgeCases(): void {
    expect(Vec\slice(vec[1, 2, 3, 4, 5], 0, 0))->toEqual(vec[]);
    expect(Vec\slice(vec[1, 2, 3], 1, 100))->toEqual(vec[2, 3]);
    expect(Vec\slice(vec[], 0, 5))->toEqual(vec[]);
  }

}

