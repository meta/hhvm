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

final class MathExtendedSuiteTest extends HackTest {

  public static function provideTrigIdentities(): vec<(float, float)> {
    return vec[
      tuple(0.0, 0.0),
      tuple(M_PI / 6.0, 0.5),
      tuple(M_PI / 4.0, M_SQRT1_2),
      tuple(M_PI / 2.0, 1.0),
      tuple(M_PI, 0.0),
      tuple(3.0 * M_PI / 2.0, -1.0),
      tuple(2.0 * M_PI, 0.0),
    ];
  }

  <<DataProvider('provideTrigIdentities')>>
  public function testSineCosinePythagorean(float $angle, float $expectedSin): void {
    $sinVal = Math\sin($angle);
    $cosVal = Math\cos($angle);
    expect($sinVal * $sinVal + $cosVal * $cosVal)->toAlmostEqual(1.0);
  }

  public static function provideExponentialLogarithmPairs(): vec<(float)> {
    return vec[
      tuple(0.1),
      tuple(0.5),
      tuple(1.0),
      tuple(2.718281828459),
      tuple(10.0),
      tuple(100.0),
      tuple(1234.5678),
    ];
  }

  <<DataProvider('provideExponentialLogarithmPairs')>>
  public function testExpLogInversion(float $val): void {
    $logVal = Math\log($val);
    $expVal = Math\exp($logVal);
    expect($expVal)->toAlmostEqual($val);
  }

  public static function provideBaseConvertExtended(): vec<(string, int, int, string)> {
    return vec[
      tuple('255', 10, 16, 'ff'),
      tuple('ff', 16, 10, '255'),
      tuple('11111111', 2, 16, 'ff'),
      tuple('ff', 16, 2, '11111111'),
      tuple('377', 8, 10, '255'),
      tuple('255', 10, 8, '377'),
      tuple('z', 36, 10, '35'),
      tuple('35', 10, 36, 'z'),
      tuple('1000000', 10, 36, 'lfl0'),
      tuple('lfl0', 36, 10, '1000000'),
    ];
  }

  <<DataProvider('provideBaseConvertExtended')>>
  public function testBaseConvertExtended(
    string $value,
    int $fromBase,
    int $toBase,
    string $expected,
  ): void {
    expect(Math\base_convert($value, $fromBase, $toBase))->toEqual($expected);
  }

  public function testLogCustomBase(): void {
    expect(Math\log(8.0, 2.0))->toAlmostEqual(3.0);
    expect(Math\log(100.0, 10.0))->toAlmostEqual(2.0);
    expect(Math\log(81.0, 3.0))->toAlmostEqual(4.0);
    expect(Math\log(1024.0, 2.0))->toAlmostEqual(10.0);
  }

  public function testLogInvariants(): void {
    expect(() ==> Math\log(0.0))->toThrow(InvariantException::class);
    expect(() ==> Math\log(-5.0))->toThrow(InvariantException::class);
    expect(() ==> Math\log(10.0, 0.0))->toThrow(InvariantException::class);
    expect(() ==> Math\log(10.0, 1.0))->toThrow(InvariantException::class);
    expect(() ==> Math\log(10.0, -2.0))->toThrow(InvariantException::class);
  }

  public static function provideRoundingModes(): vec<(float, int, float)> {
    return vec[
      tuple(1.23456, 2, 1.23),
      tuple(1.23556, 2, 1.24),
      tuple(1234.56, -2, 1200.0),
      tuple(1256.56, -2, 1300.0),
      tuple(-1.235, 2, -1.24),
      tuple(-1.234, 2, -1.23),
      tuple(0.0, 4, 0.0),
    ];
  }

  <<DataProvider('provideRoundingModes')>>
  public function testRoundingModes(float $val, int $precision, float $expected): void {
    expect(Math\round($val, $precision))->toEqual($expected);
  }

  public static function provideCeilFloorPairs(): vec<(num, float, float)> {
    return vec[
      tuple(2.3, 3.0, 2.0),
      tuple(-2.3, -2.0, -3.0),
      tuple(5.0, 5.0, 5.0),
      tuple(-5.0, -5.0, -5.0),
      tuple(0.0, 0.0, 0.0),
      tuple(0.00001, 1.0, 0.0),
      tuple(-0.00001, 0.0, -1.0),
    ];
  }

  <<DataProvider('provideCeilFloorPairs')>>
  public function testCeilAndFloor(num $val, float $expectedCeil, float $expectedFloor): void {
    expect(Math\ceil($val))->toEqual($expectedCeil);
    expect(Math\floor($val))->toEqual($expectedFloor);
  }

  public function testIntDivInvariants(): void {
    expect(Math\int_div(10, 3))->toEqual(3);
    expect(Math\int_div(-10, 3))->toEqual(-3);
    expect(Math\int_div(10, -3))->toEqual(-3);
    expect(Math\int_div(-10, -3))->toEqual(3);
    expect(Math\int_div(0, 5))->toEqual(0);

    expect(() ==> Math\int_div(10, 0))->toThrow(DivisionByZeroException::class);
  }

  public static function provideFromBaseCases(): vec<(string, int, int)> {
    return vec[
      tuple('0', 2, 0),
      tuple('1', 2, 1),
      tuple('10', 2, 2),
      tuple('1010', 2, 10),
      tuple('77', 8, 63),
      tuple('ff', 16, 255),
      tuple('FF', 16, 255),
      tuple('z', 36, 35),
      tuple('Z', 36, 35),
      tuple('10', 36, 36),
    ];
  }

  <<DataProvider('provideFromBaseCases')>>
  public function testFromBaseCases(string $str, int $base, int $expected): void {
    expect(Math\from_base($str, $base))->toEqual($expected);
  }

  public function testFromBaseInvariants(): void {
    expect(() ==> Math\from_base('', 10))->toThrow(InvariantException::class);
    expect(() ==> Math\from_base('10', 1))->toThrow(InvariantException::class);
    expect(() ==> Math\from_base('10', 37))->toThrow(InvariantException::class);
    expect(() ==> Math\from_base('2', 2))->toThrow(InvariantException::class);
    expect(() ==> Math\from_base('g', 16))->toThrow(InvariantException::class);
  }
}
