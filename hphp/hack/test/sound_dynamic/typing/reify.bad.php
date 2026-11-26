<?hh

class C<reify T> {}

function expect_Cdyn(C<dynamic> $c) : void { }

function expect_Cint(C<int> $c) : void { }

function test(C<dynamic> $cd, C<int> $ci) : void {
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, C<dynamic>>($ci);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, C<int>>($cd);
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, C<dynamic>>(new C<int>());
  \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, C<int>>(new C<dynamic>());
}
