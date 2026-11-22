<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

use namespace HH\Lib\Str;
use function HH\__Private\MiniTest\expect;
use type HH\__Private\MiniTest\{DataProvider, HackTest};

final class StrExtendedSuiteTest extends HackTest {

  public static function provideFormatAndChunkCases(): vec<(string, int, vec<string>)> {
    return vec[
      tuple('abcdefghij', 2, vec['ab', 'cd', 'ef', 'gh', 'ij']),
      tuple('abcdefghij', 3, vec['abc', 'def', 'ghi', 'j']),
      tuple('a', 5, vec['a']),
      tuple('', 3, vec[]),
      tuple('hello world', 5, vec['hello', ' worl', 'd']),
    ];
  }

  <<DataProvider('provideFormatAndChunkCases')>>
  public function testChunk(string $input, int $size, vec<string> $expected): void {
    expect(Str\chunk($input, $size))->toEqual($expected);
  }

  public function testChunkInvalidSize(): void {
    expect(() ==> Str\chunk('test', 0))->toThrow(InvariantException::class);
    expect(() ==> Str\chunk('test', -1))->toThrow(InvariantException::class);
  }

  public static function provideTrimCases(): vec<(string, ?string, string, string, string)> {
    return vec[
      tuple('  hello  ', null, 'hello', 'hello  ', '  hello'),
      tuple('---hello---', '-', 'hello', 'hello---', '---hello'),
      tuple('abcHELLOcba', 'abc', 'HELLO', 'HELLOcba', 'abcHELLO'),
      tuple('', null, '', '', ''),
      tuple('   ', null, '', '', ''),
    ];
  }

  <<DataProvider('provideTrimCases')>>
  public function testTrim(
    string $input,
    ?string $charlist,
    string $expectedBoth,
    string $expectedLeft,
    string $expectedRight,
  ): void {
    if ($charlist === null) {
      expect(Str\trim($input))->toEqual($expectedBoth);
      expect(Str\trim_left($input))->toEqual($expectedLeft);
      expect(Str\trim_right($input))->toEqual($expectedRight);
    } else {
      expect(Str\trim($input, $charlist))->toEqual($expectedBoth);
      expect(Str\trim_left($input, $charlist))->toEqual($expectedLeft);
      expect(Str\trim_right($input, $charlist))->toEqual($expectedRight);
    }
  }

  public static function providePaddingCases(): vec<(string, int, string, string, string, string)> {
    return vec[
      tuple('cat', 6, '-', '---cat', 'cat---', '-cat--'),
      tuple('cat', 5, 'ab', 'abcat', 'catab', 'acatb'),
      tuple('hello', 3, 'x', 'hello', 'hello', 'hello'),
      tuple('', 4, 'o', 'oooo', 'oooo', 'oooo'),
    ];
  }

  <<DataProvider('providePaddingCases')>>
  public function testPadding(
    string $input,
    int $length,
    string $padStr,
    string $expectedLeft,
    string $expectedRight,
    string $expectedBoth,
  ): void {
    expect(Str\pad_left($input, $length, $padStr))->toEqual($expectedLeft);
    expect(Str\pad_right($input, $length, $padStr))->toEqual($expectedRight);

    expect(Str\pad_left($input, $length, $padStr))->toEqual($expectedLeft);
  }

  public static function provideCapitalizeCases(): vec<(string, string, string, string)> {
    return vec[
      tuple('hello world', 'HELLO WORLD', 'hello world', 'Hello world'),
      tuple('hEllO WoRlD', 'HELLO WORLD', 'hello world', 'HEllO WoRlD'),
      tuple('1234!', '1234!', '1234!', '1234!'),
      tuple('', '', '', ''),
    ];
  }

  <<DataProvider('provideCapitalizeCases')>>
  public function testCaseConversions(
    string $input,
    string $upper,
    string $lower,
    string $capitalized,
  ): void {
    expect(Str\uppercase($input))->toEqual($upper);
    expect(Str\lowercase($input))->toEqual($lower);
    expect(Str\capitalize($input))->toEqual($capitalized);
  }

  public static function provideSplitAndJoinCases(): vec<(string, string, vec<string>)> {
    return vec[
      tuple('a,b,c', ',', vec['a', 'b', 'c']),
      tuple('apple::banana::cherry', '::', vec['apple', 'banana', 'cherry']),
      tuple('no_delimiter', ',', vec['no_delimiter']),
      tuple('', ',', vec['']),
    ];
  }

  <<DataProvider('provideSplitAndJoinCases')>>
  public function testSplitAndJoin(
    string $input,
    string $delimiter,
    vec<string> $expectedSplit,
  ): void {
    expect(Str\split($input, $delimiter))->toEqual($expectedSplit);
    expect(Str\join($expectedSplit, $delimiter))->toEqual($input);
  }

  public function testSplitEmptyDelimiter(): void {
    expect(() ==> Str\split('test', ''))->toThrow(InvariantException::class);
  }

  public static function provideRepeatCases(): vec<(string, int, string)> {
    return vec[
      tuple('a', 5, 'aaaaa'),
      tuple('abc', 3, 'abcabcabc'),
      tuple('abc', 0, ''),
      tuple('', 10, ''),
    ];
  }

  <<DataProvider('provideRepeatCases')>>
  public function testRepeat(string $input, int $multiplier, string $expected): void {
    expect(Str\repeat($input, $multiplier))->toEqual($expected);
  }

  public function testRepeatNegativeMultiplier(): void {
    expect(() ==> Str\repeat('test', -1))->toThrow(InvariantException::class);
  }

  public static function provideReplaceCases(): vec<(string, string, string, string)> {
    return vec[
      tuple('the quick brown fox', 'quick', 'slow', 'the slow brown fox'),
      tuple('foo bar foo baz foo', 'foo', 'qux', 'qux bar qux baz qux'),
      tuple('hello', 'world', 'there', 'hello'),
      tuple('aaa', 'a', 'bb', 'bbbbbb'),
    ];
  }

  <<DataProvider('provideReplaceCases')>>
  public function testReplace(
    string $haystack,
    string $needle,
    string $replacement,
    string $expected,
  ): void {
    expect(Str\replace($haystack, $needle, $replacement))->toEqual($expected);
  }

  public function testReplaceEmptyNeedle(): void {
    expect(() ==> Str\replace('test', '', 'x'))->toThrow(InvariantException::class);
  }
}
