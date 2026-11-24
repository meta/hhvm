<?hh

function f(int $a, /*range-start*//*range-end*/named string $b): void {}

function caller(): void {
  f(1, b = 'hello');
}
