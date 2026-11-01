<?hh

function kv(vec_or_dict<int> $v) : void {
  $v["1"] = 1;
}

<<__EntryPoint>>
function main() : void {
  $v = vec[0];
  kv($v);
}
