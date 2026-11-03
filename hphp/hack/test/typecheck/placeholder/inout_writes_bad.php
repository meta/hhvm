<?hh

function assignment(inout int $_): void { $_ = ''; }
function destructure(inout int $_): void {
  list(list($_)) = tuple(tuple(''));
}
function iteration(inout int $_): void {
  foreach (dict['key' => 'value'] as $_ => $_) {}
}
function caught(inout Exception $_): void {
  try { throw new Exception(); } catch (Exception $_) {}
}
function nested(): void {
  $f = (inout int $_): void ==> { $_ = ''; };
}
function outer(inout int $_): void {
  $f = () ==> { $_ = ''; };
  $_ = '';
}
function duplicate_inout_first(inout int $_, string $_): void { $_ = ''; }
function duplicate_inout_last(string $_, inout int $_): void { $_ = ''; }

class Writes {
  public function test(inout int $_): void { $_ = ''; }
}
