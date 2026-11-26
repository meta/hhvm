<?hh

class C {
  const type T = supportdyn<nonnull>;
}

<<__SupportDynamicType>>
class D extends C { }

<<__SupportDynamicType>>
class A { }

function expectNonNull(nonnull $nn):bool {
  return false;
}

function expectSupportDynNonNull(supportdyn<nonnull> $sd): bool {
  return expectNonNull($sd);
}

function expr(supportdyn<mixed> $sd): void {
  $sd === 1;
}

function stmt(\HH\supportdyn<mixed> $sd): void {
  if ($sd is nonnull) {}
  switch ($sd) {
    case 42:
      break;
    default:
      break;
  }
}

function test1(D $d, C $c):void {
  expectSupportDynNonNull($d);
}

function test2(supportdyn<mixed> $sd):dynamic {
  return \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($sd);
}

function test3(supportdyn<mixed> $sd):?supportdyn<nonnull> {
  return $sd;
}

function expectTrav<T as supportdyn<nonnull>>(Traversable<?T> $_):void { }

function test4(vec<supportdyn<mixed>> $v):void {
  expectTrav($v);
}

function voidfun():void { }

function callvoid():supportdyn<mixed> {
  return voidfun();
}

function returnunion(bool $b):supportdyn<nonnull> {
  return $b ? new A() : new D();
}

function test_as(mixed $m):nonnull {
  return $m as C::T;
}

function test_is(mixed $m):bool {
  return $m is C::T && expectNonNull($m);
}

function test_as_2(supportdyn<mixed> $m): supportdyn<nonnull> {
  return $m as C::T;
}

function test_is_2(supportdyn<mixed> $m): bool {
  return $m is C::T && expectSupportDynNonNull($m);
}
