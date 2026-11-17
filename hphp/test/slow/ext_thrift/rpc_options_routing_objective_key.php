<?hh

<<__EntryPoint>>
function main(): void {
  $options = new RpcOptions();
  var_dump(strpos($options->__toString(), 'routingObjectiveKey') !== false);
  var_dump($options->setRoutingObjectiveKey('peak_86') === $options);
  var_dump(
    strpos($options->__toString(), 'routingObjectiveKey: "peak_86"; ')
      !== false,
  );
}
