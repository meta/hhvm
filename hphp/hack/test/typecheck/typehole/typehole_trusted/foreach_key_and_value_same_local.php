<?hh

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  foreach (dict['k' => 1] as $a => $a) {
    takes_int($a);
  }
}
