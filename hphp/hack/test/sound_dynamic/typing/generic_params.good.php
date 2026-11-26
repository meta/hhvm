<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

<<__SupportDynamicType>>
class Box<<<__RequireDynamic>> T> {
  public function __construct(private T $item) { }
  public function get(): T { return $this->item; }
  public function set(T $x):void { $this->item = $x; }
}

<<__SupportDynamicType>>
class SimpleBox<<<__RequireDynamic>> T> {
  public function __construct(public T $item) { }
}

<<__SupportDynamicType>>
interface Getter<<<__RequireDynamic>> +T> {
  public function get(): ~T;
}

<<__SupportDynamicType>>
class ROBox<<<__RequireDynamic>> +T> implements Getter<vec<T>> {
  public function __construct(private ~vec<T> $item) { }
  public function get(): ~vec<T> { return $this->item; }
}

function getVec():~vec<int> {
  return HH\FIXME\UNSAFE_CAST<vec<string>,vec<int>>(vec["A"]);
}
<<__EntryPoint>>
function breakit():~int {
  $x = new ROBox<int>(getVec());
  return $x->get()[0];
}

<<__SupportDynamicType>>
class KeyBox<<<__RequireDynamic>> T as arraykey> {
  public function __construct(private T $item) { }
  public function get(): T { return $this->item; }
  public function set(T $x):void { $this->item = $x; }
}

function testit(Getter<float> $g):void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(new Box(3));
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(new Box(true));
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(new SimpleBox("a"));
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(new KeyBox<arraykey>(3));
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($g);
}
