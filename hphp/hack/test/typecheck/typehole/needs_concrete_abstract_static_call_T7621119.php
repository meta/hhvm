<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class SomeClass<T> {}

abstract class Parent_<T> {
  abstract public static function get(): this;
  abstract public function someClass(): SomeClass<T>;
  <<__NeedsConcrete>>
  public static function getSomeClass(): SomeClass<T> {
    $self = static::get();
    return $self->someClass();
  }
}

final class Child extends Parent_<int> {
  public static function get(): this {
    return new self();
  }
  public function someClass(): SomeClass<int> {
    return new SomeClass<int>();
  }
}

<<__EntryPoint>>
function fun(): void {
  $c = Child::getSomeClass(); // $c is SomeClass<int>
  Parent_::getSomeClass(); // splode
}
