<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

final class C {
  public function __construct(
    int $positional,
    named string $zz_named_opt_z = '',
    named int $zz_named_req_z,
    named bool $zz_named_opt_a = false,
    named float $zz_named_req_a,
    named string $zz_named_used = '',
  ) {}
}

function test_named_argument_autocomplete_in_constructor_call(): void {
  new C(1, zz_named_used='', zz_named_AUTO332);
}
