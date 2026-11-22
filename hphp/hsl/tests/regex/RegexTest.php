<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */


use namespace HH\Lib\{C, Regex, Str, Vec};

use function HH\__Private\MiniTest\expect;
use type HH\__Private\MiniTest\{DataProvider, HackTest};

final class RegexTest extends HackTest {

  public static function checkThrowsOnInvalidOffset<T>(
    (function (string, Regex\Pattern<shape(...)>, int): T) $fn,
  ): void {
    expect(() ==> $fn('Hello', re"/Hello/", 5))->notToThrow();
    expect(() ==> $fn('Hello', re"/Hello/", -5))->notToThrow();
    expect(() ==> $fn('Hello', re"/Hello/", 6))->
      toThrow(
        InvariantException::class,
        // @oss-disable: InvariantViolationException::class,
        null,
        'Invalid offset should throw an exception',
      );
    expect(() ==> $fn('Hello', re"/Hello/", -6))->
      toThrow(
        InvariantException::class,
        // @oss-disable: InvariantViolationException::class,
        null,
        'Invalid offset should throw an exception',
      );
  }

  public function testThrowsOnInvalidOffset(): void {
    self::checkThrowsOnInvalidOffset(($a, $b, $i) ==> Regex\first_match($a, $b, $i));
    self::checkThrowsOnInvalidOffset(($a, $b, $i) ==> Regex\matches($a, $b, $i));
    self::checkThrowsOnInvalidOffset(
      ($a, $b, $i) ==> Regex\every_match($a, $b, $i));
    self::checkThrowsOnInvalidOffset(($a, $b, $i) ==> Regex\replace($a, $b, $a, $i));
  }

  public static function provideFirstMatch(
  ): vec<(string, Regex\Pattern<shape(...)>, int, darray<arraykey, string>)> {
    return vec[
      tuple('abce', re"/abc(.?)e(.*)/", 0, dict[
        0 => 'abce',
        1 => '',
        2 => '',
      ]),
      tuple('abcdef', re"/abc(.?)e([fg])/", 0, dict[
        0 => 'abcdef',
        1 => 'd',
        2 => 'f',
      ]),
      tuple('abcdef', re"/abc(?P<name>def)/", 0, dict[
        0 => 'abcdef',
        'name' => 'def',
        1 => 'def',
      ]),
      tuple('foo', re"/foo(bar)?/", 0, dict[
        0 => 'foo',
        1 => '',
      ]),
      tuple('abcdef', re"/def/", 1, dict[
        0 => 'def',
      ]),
      tuple('hello', re"/(.?)/", 0, dict[
        0 => 'h',
        1 => 'h',
      ]),
      tuple('hello', re"//", 0, dict[
        0 => '',
      ]),
      tuple('', re"/(.?)/", 0, dict[
        0 => '',
        1 => '',
      ]),
      tuple('', re"//", 0, dict[
        0 => '',
      ]),
    ];
  }

  <<DataProvider('provideFirstMatch')>>
  public function testFirstMatch(
    string $haystack,
    Regex\Pattern<shape(...)> $pattern,
    int $offset,
    darray<arraykey, string> $expected,
  ): void {
    $captures = Regex\first_match($haystack, $pattern);
    $captures = expect($captures)->toNotBeNull();
    expect($captures)->toEqual($expected);
  }

  public static function provideFirstMatchNull(
  ): vec<(string, Regex\Pattern<shape(...)>, int)> {
    return vec[
      tuple('a', re"/abc(.?)e(.*)/", 0),
      tuple('abcdef', re"/abc/", 1),
      tuple('', re"/abc(.?)e(.*)/", 0),
    ];
  }

  <<DataProvider('provideFirstMatchNull')>>
  public function testFirstMatchNull(
    string $haystack,
    Regex\Pattern<shape(...)> $pattern,
    int $offset,
  ): void {
    expect(Regex\first_match($haystack, $pattern, $offset))
      ->toBeNull();
  }

  public function testRecursion(): void {
    expect(() ==> Regex\first_match(Str\repeat('a', 10000).'b', re"/a*a*a*a*a$/"))
      ->toThrow(
        Regex\Exception::class,
        'Backtrack limit error',
        'Should reach backtrack limit',
      );
  }

  public static function provideMatches(
  ): vec<(string, Regex\Pattern<shape(...)>, int, bool)> {
    return vec[
      tuple('a', re"/abc(.?)e(.*)/", 0, false),
      tuple('', re"/abc(.?)e(.*)/", 0, false),
      tuple('abce', re"/abc(.?)e(.*)/", 0, true),
      tuple('abcdef', re"/abc(.?)e([fg])/", 0, true),
      tuple('abcdef', re"/abc/", 1, false),
      tuple('abcdef', re"/def/", 1, true),
      tuple('Things that are equal in PHP', re"/php/i", 2, true),
      tuple('is the web scripting', re"/\\bweb\\b/i", 0, true),
      tuple('is the interwebz scripting', re"/\\bweb\\b/i", 0, false),
    ];
  }

  <<DataProvider('provideMatches')>>
  public function testMatches(
    string $haystack,
    Regex\Pattern<shape(...)> $pattern,
    int $offset,
    bool $expected,
  ): void {
    expect(Regex\matches($haystack, $pattern, $offset))
      ->toEqual($expected);
  }

  public static function provideEveryMatch(
  ): vec<(
    string,
    Regex\Pattern<shape(...)>,
    int,
    vec<darray<arraykey, string>>,
  )> {
    return vec[
      tuple('t1e2s3t', re"/[a-z]/", 0, vec[
        dict[0 => 't'],
        dict[0 => 'e'],
        dict[0 => 's'],
        dict[0 => 't'],
      ]),
      tuple('t1e2s3t', re"/[a-z](\d)?/", 0, vec[
        dict[0 => 't1', 1 => '1'],
        dict[0 => 'e2', 1 => '2'],
        dict[0 => 's3', 1 => '3'],
        dict[0 => 't', 1 => ''],
      ]),
      tuple('t1e2s3t', re"/[a-z](?P<digit>\d)?/", 0, vec[
        dict[0 => 't1', 'digit' => '1', 1 => '1'],
        dict[0 => 'e2', 'digit' => '2', 1 => '2'],
        dict[0 => 's3', 'digit' => '3', 1 => '3'],
        dict[0 => 't', 'digit' => '', 1 => ''],
      ]),
      tuple('test', re"/a/", 0, vec[]),
      tuple('t1e2s3t', re"/[a-z]/", 3, vec[
        dict[0 => 's'],
        dict[0 => 't'],
      ]),
      tuple('', re"//", 0, vec[
        dict[0 => ''],
      ]),
      tuple('', re"/(.?)/", 0, vec[
        dict[0 => '', 1 => ''],
      ]),
      tuple('hello', re"//", 0, vec[
        dict[0 => ''],
        dict[0 => ''],
        dict[0 => ''],
        dict[0 => ''],
        dict[0 => ''],
        dict[0 => ''],
      ]),
      tuple('hello', re"/.?/", 0, vec[
        dict[0 => 'h'],
        dict[0 => 'e'],
        dict[0 => 'l'],
        dict[0 => 'l'],
        dict[0 => 'o'],
        dict[0 => ''],
      ]),
      tuple('hello', re"//", 2, vec[
        dict[0 => ''],
        dict[0 => ''],
        dict[0 => ''],
        dict[0 => ''],
      ]),
      tuple('hello', re"/.?/", 2, vec[
        dict[0 => 'l'],
        dict[0 => 'l'],
        dict[0 => 'o'],
        dict[0 => ''],
      ]),
      tuple("<b>bold text</b><a href=howdy.html>click me</a>", re"/(<([\\w]+)[^>]*>)(.*)(<\\/\\2>)/",
        0, vec[
          dict[
            0 => "<b>bold text</b>",
            1 => "<b>",
            2 => "b",
            3 => "bold text",
            4 => "</b>",
          ],
          dict[
            0 => "<a href=howdy.html>click me</a>",
            1 => "<a href=howdy.html>",
            2 => "a",
            3 => "click me",
            4 => "</a>",
          ],
      ]),
    ];
  }

  <<DataProvider('provideEveryMatch')>>
  public function testEveryMatch(
    string $haystack,
    Regex\Pattern<shape(...)> $pattern,
    int $offset,
    vec<dict<arraykey, string>> $expected,
  ): void {
    expect(Regex\every_match($haystack, $pattern, $offset))
      ->toEqual($expected);
  }

  public static function provideReplace(
  ): vec<(string, Regex\Pattern<shape(...)>, string, int, string)> {
    return vec[
      tuple('abc', re"#d#", '', 0, 'abc'),
      tuple('abcd', re"#d#", 'e', 0, 'abce'),
      tuple('abcdcbabcdcbabcdcba', re"#d#", 'D', 4, 'abcdcbabcDcbabcDcba'),
      tuple('abcdcbabcdcbabcdcba', re"#d#", 'D', 19, 'abcdcbabcdcbabcdcba'),
      tuple('abcdcbabcdcbabcdcba', re"#d#", 'D', -19, 'abcDcbabcDcbabcDcba'),
      tuple('abcdefghi', re"#\D#", 'Z', -3, 'abcdefZZZ'),
      tuple('abcd6', re"#d(\d)#", '\1', 0, 'abc6'),
      tuple('', re"/(.?)/", 'A', 0, 'A'),
      tuple('', re"//", 'A', 0, 'A'),
      tuple('hello', re"/(.?)/", 'A', 0, 'AAAAAA'),
      tuple('hello', re"//", 'A', 0, 'AhAeAlAlAoA'),
      tuple('hello', re"//", 'A', 2, 'heAlAlAoA'),
      tuple('hello', re"//", 'A', -3, 'heAlAlAoA'),
      tuple(
        'April 15, 2003',
        re"/(\\w+) (\\d+), (\\d+)/i",
        '\${1}1,\$3',
        0,
        '${1}1,$3',
      ),
      tuple(
        'April 15, 2003',
        re"/(\\w+) (\\d+), (\\d+)/i",
        "\${1}1,\$3",
        0,
        'April1,2003',
      ),
      tuple(
        Regex\replace(
          "{startDate} = 1999-5-27",
          re"/(19|20)(\\d{2})-(\\d{1,2})-(\\d{1,2})/",
          "\\3/\\4/\\1\\2",
          0,
        ),
        re"/^\\s*{(\\w+)}\\s*=/",
        "$\\1 =",
        0,
        "\$startDate = 5/27/1999",
      ),
      tuple('ooooo', re"/.*/", 'a', 0, 'aa'),
    ];
  }

  <<DataProvider('provideReplace')>>
  public function testReplace(
    string $haystack,
    Regex\Pattern<shape(...)> $pattern,
    string $replacement,
    int $offset,
    string $expected,
  ): void {
    expect(Regex\replace($haystack, $pattern, $replacement, $offset))
      ->toEqual($expected);
  }

  public static function provideReplaceWith(): varray<mixed> {
    return vec[
      /* HH_FIXME[4297] The type of the lambda argument(s) could not be inferred */
      tuple('abc', re"#d#", $x ==> $x[0], 0, 'abc'),
      tuple('abcd', re"#d#", $x ==> 'xyz', 0, 'abcxyz'),
      tuple('abcdcbabcdcbabcdcba', re"#d#", $x ==> 'D', 0, 'abcDcbabcDcbabcDcba'),
      tuple('hellodev42.prn3.facebook.com',
        re"/dev(\d+)\.prn3(?<domain>\.facebook\.com)?/",
        /* HH_FIXME[4297] The type of the lambda argument(s) could not be inferred */
        $x ==> $x[1] . $x['domain'], 4,
        'hello42.facebook.com'),
      tuple('hellodev42.prn3.facebook.com',
        re"/dev(\d+)\.prn3(?<domain>\.facebook\.com)?/",
        /* HH_FIXME[4297] The type of the lambda argument(s) could not be inferred */
        $x ==> $x[1] . $x['domain'], 6,
        'hellodev42.prn3.facebook.com'),
      /* HH_FIXME[4297] The type of the lambda argument(s) could not be inferred */
      tuple('<table ><table >', re"@<table(\s+.*?)?>@s", $x ==> $x[1], 8, '<table > '),
      /* HH_FIXME[4297] The type of the lambda argument(s) could not be inferred */
      tuple('', re"/(.?)/", $x ==> $x[1].'A', 0, 'A'),
      /* HH_FIXME[4297] The type of the lambda argument(s) could not be inferred */
      tuple('', re"//", $x ==> $x[0].'A', 0, 'A'),
      /* HH_FIXME[4297] The type of the lambda argument(s) could not be inferred */
      tuple('hello', re"/(.?)/", $x ==> $x[1].'A', 0, 'hAeAlAlAoAA'), // unintuitive, but consistent with preg_replace_callback
      /* HH_FIXME[4297] The type of the lambda argument(s) could not be inferred */
      tuple('hello', re"//", $x ==> $x[0].'A', 0, 'AhAeAlAlAoA'),
      tuple('@[12345:67890:janedoe]', re"/@\[(\d*?):(\d*?):([^]]*?)\]/",
        /* HH_FIXME[4297] The type of the lambda argument(s) could not be inferred */
        ($x ==> Str\repeat(' ', 4 + Str\length($x[1]) + Str\length($x[2])) . $x[3] . ' '),
        0, '              janedoe '),
      tuple('ooooo', re"/.*/", $x ==> 'a', 0, 'aa'),
    ];
  }

  <<DataProvider('provideReplaceWith')>>
  public function testReplaceWith(
    string $haystack,
    Regex\Pattern<shape(...)> $pattern,
    (function(Regex\Match): string) $replace_func,
    int $offset,
    string $expected,
  ): void {
    expect(Regex\replace_with($haystack, $pattern, $replace_func, $offset))
      ->toEqual($expected);
  }

  public static function provideSplit(
  ): vec<(string, Regex\Pattern<shape(...)>, ?int, vec<string>)> {
    return vec[
      tuple('', re"/x/", null, vec['']),
      tuple('hello world', re"/x/", null, vec['hello world']),
      tuple('hello world', re"/x/", 2, vec['hello world']),
      tuple('hello world', re"/\s+/", null, vec['hello', 'world']),
      tuple('  hello world  ', re"/\s+/", null, vec['', 'hello', 'world', '']),
      tuple('  hello world  ', re"/\s+/", 2, vec['', 'hello world  ']),
      tuple('  hello world  ', re"/\s+/", 3, vec['', 'hello', 'world  ']),
      tuple('', re"/(.?)/", null, vec['', '']),
      tuple('', re"//", null, vec['', '']),
      tuple("string", re"/(.?)/", null, vec['', '', '', '', '', '', '', '']),
      tuple("string", re"//", null, vec['', 's', 't', 'r', 'i', 'n', 'g', '']),
      tuple("string", re"/(.?)/", 3, vec['', '', 'ring']),
      tuple("string", re"//", 3, vec['', 's', 'tring']),
    ];
  }

  <<DataProvider('provideSplit')>>
  public function testSplit(
    string $haystack,
    Regex\Pattern<shape(...)> $pattern,
    ?int $limit,
    vec<string> $expected,
  ): void {
    expect(Regex\split($haystack, $pattern, $limit))
      ->toEqual($expected);
  }

  public function testSplitInvalidLimit(): void {
    expect(() ==> Regex\split('hello world', re"/x/", 1))->toThrow(
      InvariantException::class,
      // @oss-disable: InvariantViolationException::class,
    );
  }

  public static function provideToString(
  ): vec<(Regex\Pattern<Regex\Match>, string)> {
    return vec[
      tuple(re"/x/", '/x/'),
      tuple(re"/hello world/", '/hello world/'),
      tuple(re"/foo[1-9]/", '/foo[1-9]/'),
      tuple(re"/ahh.*/", '/ahh.*/'),
      tuple(re"/^https?:\\/\\//i", '/^https?:\\/\\//i'),
      tuple(re"/\\d{3}-\\d{4}/", '/\\d{3}-\\d{4}/'),
    ];
  }

  <<DataProvider('provideToString')>>
  public function testToString(
    Regex\Pattern<Regex\Match> $pattern,
    string $expected,
  ): void {
    expect(Regex\to_string($pattern))->toBePHPEqual($expected);
  }
}
