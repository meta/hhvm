<?hh

<<__SupportDynamicType>>
class C<T as supportdyn<mixed>> {
  public function __construct(private ~T $x) {}
  public function get() : ~T { return $this->x; }

  public function k((function (~T) : T) $f) : T {
    return $f($this->x);
  }
}

function f() : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(new C<int>(1));
}
