<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

function just_return_it<T>((function(T):void) $f):(function(T):void) { return $f; }

function break_it(string $ss):void {
  $g1 = just_return_it($y ==> $y->this_method_does_not_exist());
  $g2 = just_return_it($y ==> { $z = $y; $z->this_method_also_does_not_exist(); });
  $g1("hahahaha");
}
