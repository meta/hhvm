<?hh

<<file:__EnableUnstableFeatures('require_constraint')>>

class C {
    use T;

    protected function bar(): void {}
}

trait T {
    require class C;

    public function foo(): void {
        $this->bar();
    }
}
