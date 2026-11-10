<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

// These spellings are already accepted at named argument call sites.
function supported_names(
  named int $module_,
  named int $async_,
  named int $readonly_,
  named int $internal_,
  named int $optional_,
  named int $type_,
): int {
  return $module_ + $async_ + $readonly_ + $internal_ + $optional_ + $type_;
}

// Positional parameter names are less restricted than named parameter names
// `function foo(named int $module): void` is not allowed but `function foo(int $module): void` is allowed
function positional_names(int $module, int $namespace, int $private): int {
  return $module + $namespace + $private;
}

class SupportedParameterNames {
  public function __construct(public named int $module_) {}
  public function method(named int $async_): int {
    return $this->module_ + $async_;
  }
}

type SupportedParameterType = (function(named int $module_): int);

function call_supported(): int {
  $closure = function(named int $module_): int { return $module_; };
  $lambda = (named int $async_): int ==> $async_;
  $object = new SupportedParameterNames(module_=1);
  return supported_names(
    module_=1,
    async_=2,
    readonly_=3,
    internal_=4,
    optional_=5,
    type_=6,
  ) + positional_names(1, 2, 3) + $closure(module_=4) + $lambda(async_=5) +
    $object->method(async_=6);
}
