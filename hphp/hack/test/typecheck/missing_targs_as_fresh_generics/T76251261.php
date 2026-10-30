<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class MyException<T> extends Exception {
  public function __construct(public T $in) {
    parent::__construct();
  }
}

<<__EntryPoint>>
function foo(): void {
  try {
    throw new MyException(42);
  } catch (MyException $e) {
    expects_string($e->in); // splode
  }
}

function expects_string(string $in): void {}
