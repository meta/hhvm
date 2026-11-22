<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

use namespace HH\Lib\{PseudoRandom, SecureRandom, Str};
use function HH\__Private\MiniTest\expect;
use type HH\__Private\MiniTest\HackTest;

final class RandomExtendedSuiteTest extends HackTest {

  public function testPseudoRandomIntBounds(): void {
    for ($i = 0; $i < 50; $i++) {
      $val = PseudoRandom\int(10, 20);
      expect($val >= 10)->toBeTrue();
      expect($val <= 20)->toBeTrue();
    }
  }

  public function testPseudoRandomFloatRange(): void {
    for ($i = 0; $i < 50; $i++) {
      $val = PseudoRandom\float();
      expect($val >= 0.0)->toBeTrue();
      expect($val <= 1.0)->toBeTrue();
    }
  }

  public function testPseudoRandomStringAlphabet(): void {
    $alphabet = 'ABCDEF';
    $str = PseudoRandom\string(32, $alphabet);
    expect(Str\length($str))->toEqual(32);
    for ($i = 0; $i < Str\length($str); $i++) {
      expect(Str\contains($alphabet, $str[$i]))->toBeTrue();
    }
  }

  public function testSecureRandomIntBounds(): void {
    for ($i = 0; $i < 20; $i++) {
      $val = SecureRandom\int(100, 200);
      expect($val >= 100)->toBeTrue();
      expect($val <= 200)->toBeTrue();
    }
  }

  public function testSecureRandomBytesLength(): void {
    $bytes = SecureRandom\bytes(16);
    expect(Str\length($bytes))->toEqual(16);
  }

  public function testSecureRandomStringAlphabet(): void {
    $alphabet = '0123456789';
    $str = SecureRandom\string(10, $alphabet);
    expect(Str\length($str))->toEqual(10);
    for ($i = 0; $i < Str\length($str); $i++) {
      expect(Str\contains($alphabet, $str[$i]))->toBeTrue();
    }
  }
}
