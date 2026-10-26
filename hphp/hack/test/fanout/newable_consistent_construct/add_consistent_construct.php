//// base-C.php
<?hh
class C {
  public function __construct() {}
}

//// base-use.php
<?hh
/** Adding __ConsistentConstruct must recheck and repair this invalid newable bound. */
function use_c<<<__Newable>> reify T as C>(): void {}

//// changed-C.php
<?hh
<<__ConsistentConstruct>>
class C {
  public function __construct() {}
}
