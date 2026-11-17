<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

function add<T>(vec<Func<T>> $rules, T $val):void {
  $rules[0]->apply($val);
}

abstract class Func<-T> {
  abstract public function apply(T $x):void;
}

final class WrappedLambda<T> extends Func<T>
{
  public function __construct(private (function(T): void) $func) { }
  public function apply(T $x): void {
    $f = $this->func;
    ($f)($x);
  }
}
function testit():void {
  add(vec[new WrappedLambda($m ==> $m->rubbish())], 4);
}
