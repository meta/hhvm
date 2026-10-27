<?hh

<<__EntryPoint>>
function main(): void {
  $values = vec[];
  for ($i = 512; $i >= 0; --$i) {
    $values[] = (string)$i;
  }

  $collator = new Collator('en_US');
  var_dump($collator->sortWithSortKeys(inout $values));
  var_dump(count($values));
}
