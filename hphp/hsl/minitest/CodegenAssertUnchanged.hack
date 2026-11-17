/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

namespace HH\__Private\MiniTest;

use namespace HH\Lib\{C, Vec, Str};

trait CodegenAssertUnchanged {
  require extends HackTest;

  const string EXTENSION = '.codegen';
  const string SEPARATOR = '!@#$%codegentest:';

  private static ?classname<mixed> $className = null;
  private static ?dict<string, string> $expected = null;

  final protected static function setupForCodegen(): void {
    self::$className = static::class;
  }

  final protected static function tearDownForCodegen(): void {
    self::$className = null;
    self::$expected = null;
  }

  /**
   * Main functionality for children classes
   *
   * $case determines distinct files to be generated
   * $token determines sections within the generated file for the same case
   */
  protected static function assertUnchanged(
    string $value,
    ?string $token = null,
  ): void {
    $token = $token ?? self::findToken();
    $exp = self::getTokenToExpected()[$token];
    expect($exp)->toEqual(
      $value,
      "The value for token (%s) has changed.\nExpected: %s\nActual: %s",
      $token,
      $exp,
      $value,
    );
  }

  private static function getClassName(): classname<mixed> {
    invariant(
      self::$className !== null,
      'setupForCodegen was not called. If you are defining setUpForRecord '.
      'in your test, then you need to call self::setupForRegen() yourself.',
    );
    return self::$className;
  }

  private static function getTokenToExpected(): dict<string, string> {
    if (self::$expected is null) {
      self::$expected = self::parseFile(self::getPath(self::getClassName()));
    }

    return self::$expected;
  }

  /**
   * Given a class name, return the uri where the codegen file should
   * be written (the uri where the class is defined -php+codegen).
   */
  private static function getPath(classname<mixed> $class_name): string {
    $ref = new \ReflectionClass(\HH\classname_to_class($class_name));
    $source_file = $ref->getFileName();
    // Get classname without namespace
    $filename = $ref->getShortName();
    return \dirname((string)$source_file).'/'.$filename.self::EXTENSION;
  }

  /**
   * Users of this trait can use whatever token they want,
   * but a common case if to use the name of the test function.
   * This returns the name of the first function that starts with 'test'
   * in the current stack trace.
   */
  private static function findToken(): string {
    $token = null;
    // Get caller function name
    $stack = \debug_backtrace();
    foreach ($stack as $function) {
      $function_name = Shapes::at($function, 'function');
      if (Str\starts_with($function_name, 'test')) {
        $token = $function_name;
        break;
      }
    }
    invariant(
      $token !== null,
      'Test framework was unable to find a function starting with '.
      '"test" when looking through the stack.',
    );
    return $token;
  }

  /**
   * Parse an existing codegen file
   */
  private static function parseFile(string $file_name): dict<string, string> {
    $map = dict[];

    if (!\file_exists($file_name)) {
      return $map;
    }

    $lines = \file($file_name);
    invariant(
      $lines !== false,
      'Fail to open the file %s for reading',
      $file_name,
    );

    $generated = \array_shift(inout $lines);
    invariant(
      Str\trim_right((string)$generated) === '@'.'generated',
      'Codegen test record file should start with a generated tag',
    );

    $token = null;
    $expected = '';
    foreach ($lines as $line) {
      if (Str\starts_with($line, self::SEPARATOR)) {
        if ($token !== null) {
          // We always add 1 newline at the end
          $expected = \substr($expected, 0, -1);
          $map[$token] = $expected;
        }
        // Format is separator:token\n
        $token = \substr(
          Str\trim_right((string)$line),
          Str\length(self::SEPARATOR),
        );
        $expected = '';
        continue;
      }

      $expected .= self::unescapeTokens($line);
    }
    if ($token !== null) {
      // We always add 1 newline at the end
      $expected = \substr($expected, 0, -1);
      $map[$token] = $expected;
    }
    return $map;
  }

  /**
   * Escape the tokens that carry signatures, so that when writing those to
   * the .codegen file, it doesn't seem like that's the file signature.
   */
  private static function escapeTokens(string $s): string {
    return Str\replace_every(
      $s,
      dict[
        '@'.'generated' => '@-generated',
        '@'.'partially-generated' => '@-partially-generated',
        '@'.'nocommit' => '@-nocommit',
      ],
    );
  }

  private static function unescapeTokens(string $s): string {
    return Str\replace_every(
      $s,
      dict[
        '@-generated' => '@'.'generated',
        '@-partially-generated' => '@'.'partially-generated',
        '@-nocommit' => '@'.'nocommit',
      ],
    );
  }
}
