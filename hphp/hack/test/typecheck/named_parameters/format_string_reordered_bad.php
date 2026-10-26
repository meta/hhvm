<?hh
<<file:__EnableUnstableFeatures('named_parameters')>>

interface Fmt {
  public function format_d(int $n): string;
  public function format_s(string $s): string;
}

function f(named \HH\FormatString<Fmt> $fmt, mixed ...$args): void {}

function g(\HH\FormatString<Fmt> $fmt, mixed ...$args): void {}

function test_reordered_bad(): void {
  f("%s", fmt = "%d");
}

function test_named_first_bad(): void {
  f(fmt = "%d", "%s");
}

function test_positional_bad(): void {
  g("%d", "s");
}

function k(named int $tag, \HH\FormatString<Fmt> $template, mixed ...$values): void {}

function m(named int $tag, named \HH\FormatString<Fmt> $fmt, mixed ...$values): void {}

function test_named_middle_bad(): void {
  k("%d", tag = 1, "s");
  k(tag = 1, "%d", "s");
  k("%d", "s", tag = 1);
}

function test_mixed_named_order_bad(): void {
  m("%s", tag = 1, fmt = "%d");
  m(fmt = "%d", "%s", tag = 1);
  m(tag = 1, fmt = "%d", "%s");
}
