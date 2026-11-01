<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

abstract class MyTestBase {
  abstract public static function get(): string;
}

abstract final class MyTestConcrete extends MyTestBase {
    <<__Override>>
      public static function get(): string {
      return 'foo';
  }
}

abstract final class MyTestRunner {
  const vec<classname<MyTestBase>> TESTS =
    vec[nameof MyTestConcrete, nameof MyTestBase];

  public static function test(): void {
    foreach (self::TESTS as $classname) {
      echo $classname::get();
    }
  }
}

<<__EntryPoint>>
function main():void {
  MyTestRunner::test();
}
