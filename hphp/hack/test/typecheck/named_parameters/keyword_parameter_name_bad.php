<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

function module_parameter(named int $module): void {}
function namespace_parameter(named int $namespace): void {}
function contextual_parameters(named int $type, named int $match): void {}
function uppercase_parameter(named int $PRIVATE): void {}
function decorated_parameter(named int $inout): void {}
function default_keyword_parameter(named int $optional = 0): void {}

class KeywordParameters {
  public function __construct(public named int $private) {}
  public function method(named int $abstract): void {}
}

interface KeywordParameterInterface {
  public function method(named int $namespace): void;
}

function closures(): void {
  $_ = function(named int $module): void {};
  $_ = (named int $private) ==> $private;
}

type KeywordParameterType = (function(named int $abstract): void);
type OptionalKeywordParameterType =
  (function(optional named int $module): void);
