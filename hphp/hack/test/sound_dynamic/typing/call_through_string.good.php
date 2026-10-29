<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

<<__SupportDynamicType>>
class C {
  public static function foo():void { }
}

<<__SupportDynamicType>>
class D {
  public function get():(~class<C> & string) {
    return HH\classname_to_class(C::class);
  }
}

<<__SupportDynamicType>>
function test(vec<int> $_):void {
  $d = new D();
  $c = $d->get();
  $c::foo();
}
