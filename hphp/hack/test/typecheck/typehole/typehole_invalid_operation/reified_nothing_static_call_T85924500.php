<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

function foo<reify T>(): void where T = nothing {
  T::stuff();
}

  <<__EntryPoint>>
  function bar(): void {
    foo<nothing>();
}
