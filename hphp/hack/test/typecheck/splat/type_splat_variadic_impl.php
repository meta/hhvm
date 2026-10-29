<?hh


interface Splat<T as (string...)> {
  public function __construct(... T $values);
}

// A pure variadic (no fixed parameters) satisfies a pure splat.
final class Impl implements Splat<(string...)> {
  public function __construct(string ...$values)[] {}
}

// A splat with a leading fixed element also checks fine.
final class Impl2 implements Splat<(string, string...)> {
  public function __construct(string $first, string ...$rest)[] {}
}
