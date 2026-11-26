<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

function expectSupportDynamic(supportdyn<nonnull> $_):void { }
function expectDynamic(dynamic $_):void { }
function expectPrecise<T1,T2>((function(T1):T2) $_):void { }
function expectLike<T1,T2>(~(function(T1):T2) $_):void { }

// Explicit SDT lambda, enforced parameter type
function explicit_sdt_enforced():void {
  $f = <<__SupportDynamicType>> (int $x) ==> true;
  expectSupportDynamic($f);
  expectDynamic(\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f));
  expectPrecise<int,bool>($f);
  expectLike<int,bool>($f);
}

// Implicit SDT lambda, enforced parameter type
<<__SupportDynamicType>>
function implicit_sdt_enforced():void {
  $f = (int $x) ==> true;
  expectSupportDynamic($f);
  expectDynamic(\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f));
  expectPrecise<int,bool>($f);
  expectLike<int,bool>($f);
}

function intpred(int $x):bool { return false; }

// Implicit SDT lambda, inferred return type
<<__SupportDynamicType>>
function implicit_inferred():void {
  $f = (int $x) ==> intpred($x);
  expectSupportDynamic($f);
  expectDynamic(\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f));
  expectPrecise<int,bool>($f);
  expectLike<int,bool>($f);
}

// Explicit SDT lambda, unenforced explicit parameter type
function explicit_nonenforced():void {
  $f = <<__SupportDynamicType>> (vec<int> $x) ==> $x[0];
  expectSupportDynamic($f);
  expectDynamic(\HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>($f));
  expectPrecise<vec<int>,int>($f);
  expectLike<vec<int>,int>($f);
}
