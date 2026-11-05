<?hh

// Called directly in a sync entry point, there is no caller frame.
<<__EntryPoint>>
function main(): void {
  var_dump(hphp_debug_caller_info());
}
