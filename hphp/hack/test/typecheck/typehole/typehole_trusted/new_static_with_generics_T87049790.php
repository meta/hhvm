<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__ConsistentConstruct>>
abstract class C1<T> {
  final public function __construct(private T $value) {}

  <<__NeedsConcrete>>
  public static function from(mixed $value): this {
    return new static($value);
  }

  public function getValue() : T {
    return $this->value;
  }
}

final class C2 extends C1<string> {}

<<__EntryPoint>>
function main() : string {
  return C2::from(10)->getValue();
}
