<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

class EAttr implements HH\EnumAttribute {
  public function __construct(public int $i) {}
}

class FAttr implements HH\FunctionAttribute {
  public function __construct(public int $j) {}
}

<<EAttr(1)>>
enum E: int {}

function x(): void {
  $rc = new ReflectionClass(E::class);
  $rc->getAttributeClass(EAttr::class);
  $rc->getAttributeClass(FAttr::class);
}
