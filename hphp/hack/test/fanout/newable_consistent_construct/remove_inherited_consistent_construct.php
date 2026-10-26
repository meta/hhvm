//// base-Parent.php
<?hh
<<__ConsistentConstruct>>
class ParentClass {
  public function __construct() {}
}

//// base-Inherited.php
<?hh
class Inherited extends ParentClass {}

//// base-Local.php
<?hh
class Local extends ParentClass {
  public function __construct() {
    parent::__construct();
  }
}

//// base-use.php
<?hh
/** A newable bound must track consistency inherited from its constructor's origin. */
function use_inherited<<<__Newable>> reify T as Inherited>(): void {}

/** Inherited consistency also matters when the bound declares its own constructor. */
function use_local<<<__Newable>> reify T as Local>(): void {}

//// changed-Parent.php
<?hh
class ParentClass {
  public function __construct() {}
}
