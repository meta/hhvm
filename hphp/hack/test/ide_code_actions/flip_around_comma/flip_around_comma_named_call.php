<?hh

function bar(named int $a, named int $b): void {}

function caller(): void {
  bar(a = 1, /*range-start*//*range-end*/b = 2);
}
