<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.


class Box<T> {
  public function __construct(private T $x) {}
  public function get() : T { return $this->x; }
  public function set(T $x) : void { $this->x = $x; }
}

<<__SupportDynamicType>>
class ROBox<T> {
  public function __construct(private T $x) {}
  public function get() : T { return $this->x; }
}

<<__SupportDynamicType>>
function f(Box<int> $v1, ROBox<int> $v2) : void {
  $v1->set($v2->get());
}

<<__EntryPoint>>
function main() : void {
  $v1 = new Box<int>(1);
  $v2 = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(new ROBox<string>("abcd"));
  f($v1, $v2);
  print $v1->get();
}
