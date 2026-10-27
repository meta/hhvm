<?hh

function makeNonStatic($n) :mixed{
  $s = 'foobar';
  for ($i = 0; $i < $n; $i += 1) {
    $s .= $i;
  }
  return $s;
}


// Runs twice with Treadmill in between (see .config)
<<__EntryPoint>>
function main_uncounted_array_copy() :mixed{
if (apc_exists('minefield')) {
  // Second run: verify array was correctly copied.
  var_dump(__hhvm_intrinsics\apc_fetch_no_check('minefield'));
} else {
  // First run: Create array with uncounted, non-static string value.
  $ns = makeNonStatic(10);
  apc_store('mine', $ns, 3);
  $unc0 = __hhvm_intrinsics\apc_fetch_no_check('mine');
  $unc1 = __hhvm_intrinsics\apc_fetch_no_check('mine');
  $v = dict[];
  $v['hey'] = $unc0;
  $v[0] = $unc1;
  var_dump($v);
  // This should recursively copy all of 'v' before storing in APC...
  apc_store('minefield', $v, 100);
  // ...since its values can easily go away (after Treadmill runs):
  apc_delete('mine');
}
}
