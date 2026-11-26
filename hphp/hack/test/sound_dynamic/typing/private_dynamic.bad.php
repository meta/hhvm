<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

class D { }

<<__SupportDynamicType>>
class C {
  // Legal, as we don't check signatures for private members
  private vec<int> $items;
  private function setItems(vec<int> $vi):void {
    // Unfortunately illegal, as setItems might be called through dynamic
    $this->items = $vi;
  }

  public function __construct() { $this->items = vec[]; }
  public function getItems(): vec<int> {
    return $this->items;
  }
  public function isMember(int $x):bool {
    return false;
  }
  private function bad1(dynamic $d):void {
    // Illegal, as there exists a private member items of non-enforceable type
    $d->items = vec["A"];
  }
}

<<__SupportDynamicType>>
class Wrap {
  // Legal, as we don't check signatures for private members
  private D $item;
  public function __construct() { $this->item = new D(); }
  private function bad2(dynamic $d):dynamic {
    // Illegal, as there exists a private member items of type not subtype of dynamic
    return $d->item;
  }
}

class NonDyn {
  private D $item1;
  private vec<int> $item2;
  public function __construct() { $this->item1 = new D(); $this->item2 = vec[]; }
  public function bad3(dynamic $d):void {
    $d->item2 = vec["A"];
  }
  public function bad4(dynamic $d):dynamic {
    return $d->item1;
  }
  public function get():int {
    return $this->item2[0];
  }
}

<<__SupportDynamicType>>
class Dyn extends NonDyn { }

// Demonstrates why we need the checks above
<<__EntryPoint>>
function main():void {
  $d = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(new Dyn());
  $d->bad3($d);
  $x = $d->get();
}
