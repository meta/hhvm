<?hh

<<file:__EnableUnstableFeatures('require_constraint')>>

class C {
    public static function bar(): void {}
}

class D extends C {
    use T;
}

trait T {
    /* HH_IGNORE[12043] require-class constraint; class does not use trait */
    require class C;

    public function foo(): void {
        parent::bar();
    }
}
