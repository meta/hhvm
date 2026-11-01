<?hh

<<__EntryPoint>>
function main(): void {
  $x = vec<KeyedContainer<arraykey, string>>[vec[]][0];
  $x['name'];
}
