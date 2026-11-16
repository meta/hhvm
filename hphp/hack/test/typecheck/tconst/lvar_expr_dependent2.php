<?hh

// This file tests expression dependent types for locals variables
// when integration occurs

abstract class C {
  abstract const type T as num;

  abstract public function get(): this::T;
  abstract public function get_a_C(): C;

  public function test_reset_expr_dispaly_ids(C $c): void {
    // Expression Display Ids should be reset within method bo
  }

  public function test(Vector<C> $vec): void {
    $c1 = $vec[0]; $c2 = $vec[1];
    // $c1 and $c2 have different expression ids
    hh_show($c1->get());
    hh_show($c2->get());

    if ($c1 < $c2) {
      // $c will have a different expression id as $c1 in this branch
      $c = $c1;
      hh_show($c->get());
    } else {
      // $c will have a different expression id as $c2 in this branch
      $c = $c2;
      hh_show($c->get());
    }

    // After integration $c's expression id will be different than
    // the expression id for $c1 or $c2
    hh_show($c->get());

    // But the expression id is still stable
    hh_show($c->get());

    if ($c->get() === 0) {
      // Assigning to $c will produce a new expression id
      $c = $this->get_a_C();
      hh_show($c->get());
    }

    // But doing so will generate a new expression id for $c outside the block
    hh_show($c->get());

    // Yet it is still stable
    hh_show($c->get());

    $f = function(C $c): void {
      // $c in another context should have a different expression id
      hh_show($c->get());
    };

    // But leave the expression id of $c unchanged
    hh_show($c->get());

    // assigning in a for loop changes $c to a new expression id
    foreach ($vec as $c) {
      hh_show($c->get());
      // and its stable
      hh_show($c->get());
    }

    // but changes the expression id of $c outside the loop as well
    hh_show($c->get());
  }
}
