<?hh

function unused(inout int $_): void {}
function duplicates(int $_, string $_): void {}
abstract class PlaceholderBase {
  abstract public function unused(inout int $_): void;
}

function discard(): void {
  $_ = 1;
  $_ = '';
  list($_, $_) = tuple(1, '');
  list(list($_)) = tuple(tuple(1));
  foreach (dict['key' => 'value'] as $_ => $_) {}
  try {} catch (Exception $_) {}
  $_named = 1;
  $read = $_named;
}

class StaticProperty {
  public static int $_ = 0;
  public static function write(inout int $_): void {
    self::$_ = 1;
  }
}

function closure_scopes(inout int $_): void {
  $bound = function(int $_): void { $_ = ''; };
  $unused = (inout int $_): void ==> {};
  $duplicate = (int $_, string $_): void ==> {};
  $local = () ==> { $_ = ''; };
}
