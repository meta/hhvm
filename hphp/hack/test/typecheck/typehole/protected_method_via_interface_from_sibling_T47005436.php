<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

interface IExampleFailure {
  protected static function getEmptyData(): vec<string>;
  protected function get(): vec<string>;
}
abstract class BaseClass implements IExampleFailure {
  <<__NeedsConcrete>>
  public static function start(): void {
    static::getEmptyData();
  }
}
final class Foo extends BaseClass {
    <<__Override>>
      final protected static function getEmptyData(): vec<string> {
          return vec['hello!'];
  }
  <<__Override>>
      final protected function get(): vec<string> {
      return vec[];
    }
}
final class Bar extends BaseClass {
    <<__Override>>
      protected static function getEmptyData(): vec<string> {
      return Foo::getEmptyData(); // should error
    }
      <<__Override>>
      protected function get(): vec<string> {
      $x = new Foo();
      return $x->get(); // should error
    }
}

<<__EntryPoint>>
function main():void {
  Bar::start();
}
