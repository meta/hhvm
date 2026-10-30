<?hh

<<file:__EnableUnstableFeatures('require_constraint')>>

class C {
    use T;

    public static function bar(): void { echo "I am bar in C"; }
}

trait T {
    require class C;

    public function foo(): void {
        self::bar();
    }
}

<<__EntryPoint>>
function main(): void {
    (new C())->foo();
}
