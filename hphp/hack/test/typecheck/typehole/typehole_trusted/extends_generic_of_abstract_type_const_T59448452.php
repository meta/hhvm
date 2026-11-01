<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class Box<T> {
  public function __construct(private T $data) {}
  public function get(): T { return $this->data; }
  public function set(T $x): void { $this->data = $x; }
}

abstract class C extends Box<this::T> {
  abstract const type T as arraykey;
}
class CS extends C { const type T = string; }
class CI extends C { const type T = int; }

class Cov<+T> {
  public function __construct(private T $data) {}
  public function boxed(): T { return $this->data; }
}

function QCf0<T>(Cov<Box<T>> $b1, Cov<Box<T>> $b2): void {
  $b1->boxed()->set($b2->boxed()->get());
}

function QCf1(Cov<C> $b1, Cov<C> $b2): void {
  QCf0($b1, $b2);
}

<<__EntryPoint>>
function QCf2(): string {
  $b1 = new CS("Hi");
  $b2 = new CI(42);
  QCf1(new Cov($b1), new Cov($b2));
  return $b1->get();
}
