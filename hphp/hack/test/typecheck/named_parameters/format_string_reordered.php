<?hh
<<file:__EnableUnstableFeatures('named_parameters')>>

interface Fmt {
  public function format_d(int $n): string;
  public function format_s(string $s): string;
}

function f(named \HH\FormatString<Fmt> $fmt, mixed ...$args): void {}

function g(\HH\FormatString<Fmt> $fmt, mixed ...$args): void {}

function test_reordered_good(): void {
  f(42, fmt = "%d");
}

function test_named_first_good(): void {
  f(fmt = "%d", 42);
}

function test_positional_good(): void {
  g("%d", 42);
}

function k(named int $tag, \HH\FormatString<Fmt> $template, mixed ...$values): void {}

function m(named int $tag, named \HH\FormatString<Fmt> $fmt, mixed ...$values): void {}

function test_named_middle_good(): void {
  k("%d", tag = 1, 42);
  k(tag = 1, "%d", 42);
  k("%d", 42, tag = 1);
}

function test_mixed_named_order_good(): void {
  m(42, tag = 1, fmt = "%d");
  m(fmt = "%d", 42, tag = 1);
  m(tag = 1, fmt = "%d", 42);
}
