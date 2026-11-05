<?hh

// The uncaught-exception handler runs after the stack has unwound.
function on_exception(Throwable $_): void {
  var_dump(hphp_debug_caller_info());
}

<<__EntryPoint>>
function main(): void {
  set_exception_handler(on_exception<>);
  throw new Exception('unhandled');
}
