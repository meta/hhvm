<?hh

// A shutdown function runs with no Hack frame below it.
function on_shutdown(): void {
  var_dump(hphp_debug_caller_info());
}

<<__EntryPoint>>
function main(): void {
  register_shutdown_function(on_shutdown<>);
}
