<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

  class Foo<reify TData as ?int> {
    public function __construct(TData $x) {}
  }
  class Hmm {
    const type T = int;
    public function breakIt(): Foo<self::T> {
      return new Foo<int>(4);
    }
  }
<<__EntryPoint>>
function main(): void {
  $z = new Hmm();
  $z->breakIt();
  }
