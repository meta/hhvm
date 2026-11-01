<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

interface I1 {
  public function takes_t<T>(T $v): void;
}

class C1<T> implements I1 {
  public function __construct(
    public (function(T): void) $checkFn,
  ): void {}

  public function takes_t(T $v): void {
    $check_fn = $this->checkFn;
    $check_fn($v);
  }
}

function takes_i1(I1 $i): void {
  $i->takes_t("foo");
}

<<__EntryPoint>>
function create_c1(): void {
  $c = new C1((int $_) ==> {}); // inferred as C1<nothing>
  takes_i1($c);
}
