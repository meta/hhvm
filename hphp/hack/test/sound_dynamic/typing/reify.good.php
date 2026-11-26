<?hh

<<__SupportDynamicType>>
class C<reify T> {}

function test(dynamic $d, C<int> $c) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($c);
}
