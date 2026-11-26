<?hh

<<__SupportDynamicType>>
class C<T> {
  public function __construct(private T $x) {}
  public function get() : T { return $this->x; }
}

<<__SupportDynamicType>>
class D<T> {
  public function __construct(private T $x) {}
  public function set(T $x) : void { $this->x = $x; }
}

function f() : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(new C<mixed>(1));
}
