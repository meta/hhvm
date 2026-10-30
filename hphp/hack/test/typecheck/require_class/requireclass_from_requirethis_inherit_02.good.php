<?hh

<<file:__EnableUnstableFeatures('require_constraint')>>

class C {
    use T3;

    public function bar(): void {}
}

trait T1 {
    require class C;
}

trait T2 {
    use T1;
}

trait T3 {
    use T1;

    public function foo(): void {
        $this->bar();
    }
}
