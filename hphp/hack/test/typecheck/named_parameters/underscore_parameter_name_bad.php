<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

function underscore_parameter(named int $_): void {}

class UnderscoreParameter {
  public function __construct(named int $_) {}
}

function underscore_lambda(): void {
  $_ = (named int $_) ==> {};
}
