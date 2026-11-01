<?hh

// Constant keys are assumed distinct from literal keys, so indexing with
// ReproKey::FOO ('foo') types the ?int field as null.
enum ReproKey: string as string {
  FOO = 'foo';
}
type TReproShape = shape(
  ?'foo' => int,
);
function takes_nullable_string(?string $_x): void {}
function repro(TReproShape $s): void {
  takes_nullable_string(Shapes::idx($s, ReproKey::FOO)); // no error, but should be ?int
}

<<__EntryPoint>>
function main(): void {
  repro(shape('foo' => 1));
}
