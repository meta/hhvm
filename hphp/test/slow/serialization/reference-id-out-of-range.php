<?hh

// Reference ids must be bounds-checked as 64-bit values. 4294967298 is
// 2**32 + 2 and -4294967294 is -2**32 + 2; both alias id 2 if truncated to
// 32 bits.
<<__EntryPoint>>
function main(): void {
  $ids = vec[
    '2',
    '4294967298',
    '-4294967294',
    '0',
    '9223372036854775807',
  ];
  foreach ($ids as $id) {
    foreach (vec['R', 'r'] as $kind) {
      var_dump(unserialize('a:2:{i:0;s:1:"x";i:1;'.$kind.':'.$id.';}'));
    }
  }
}
