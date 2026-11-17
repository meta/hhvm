<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

class CAttr implements HH\ClassAttribute {
  public function __construct(public int $i) {}
}

class FAttr implements HH\FunctionAttribute {
  public function __construct(public int $j) {}
}

<<CAttr(1)>>
class C {}

function x(): void {
  $rc = new ReflectionClass(C::class);
  $rc->getAttributeClass(CAttr::class);
  $rc->getAttributeClass(FAttr::class);
}
