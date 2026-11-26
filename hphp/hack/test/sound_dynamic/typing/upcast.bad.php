<?hh

function f(mixed $x) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x);
}

<<__SupportDynamicType>>
class MyVector<T as supportdyn<mixed>> {}

function h(MyVector<int> $x) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, MyVector<dynamic>>($x);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($x);
}
