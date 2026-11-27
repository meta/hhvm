<?hh

<<__EntryPoint>>
function main(): void {
  foreach (vec['', 'bad-config-pid'] as $pid) {
    try {
      new MCRouter(dict['config_str' => '{"route":"NoSuchRoute"}'], $pid);
      echo "Unexpectedly initialized MCRouter\n";
    } catch (MCRouterException $e) {
      var_dump(strpos($e->getMessage(), 'NoSuchRoute') !== false);
      var_dump($e->getOp() === MCRouter::mc_op_unknown);
    }
  }
}
