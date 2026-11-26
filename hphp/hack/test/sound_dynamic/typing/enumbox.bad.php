<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.


enum E : int as int { A = 3; }
enum F : int as int { B = 5; }

<<__SupportDynamicType>>
class EBox {
  public function __construct(public E $item) { }
}

<<__EntryPoint>>
function main():void {
  $d = new EBox(E::A);
  $dd = \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($d);
  $dd->item = F::B;
  $x = $d->item;
  // Now I think I've got an E but I've actually got an F
}
