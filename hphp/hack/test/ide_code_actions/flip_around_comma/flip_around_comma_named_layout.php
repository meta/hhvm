<?hh

function f(
  named /* keep the label */ int
  $a = 1,
  /*range-start*//*range-end*/named string $b = 'hello',
): void {}

function caller(): void {
  f(a = 1, b = 'hello');
}
