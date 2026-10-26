//// base-C.php
<?hh
<<__ConsistentConstruct>>
class C {
  public function __construct() {}
}

//// base-use.php
<?hh
/**
 * Removing __ConsistentConstruct must recheck this newable bound.
 * The empty body prevents a constructor call from adding the missing dependency.
 */
function use_c<<<__Newable>> reify T as C>(): void {}

//// changed-C.php
<?hh
class C {
  public function __construct() {}
}
