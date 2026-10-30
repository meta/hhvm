<?hh

<<file:__EnableUnstableFeatures('require_constraint')>>

interface I {
    use T;

    public function bar(): void;
}

trait T {
    require class I;

    public function foo(): void {
        $this->bar();
    }
}
