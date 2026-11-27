<?hh

// Regression test: with Eval.JitPGO=false a function is translated live before
// the unit defining the constants it reads has been loaded. In
// RepoAuthoritative mode the JIT used to bind a normal RDS handle for such a
// constant, which made the persistent bind in Constant::def fail when the unit
// was autoloaded, leaving the constant undefined.

<<__NEVER_INLINE>>
function read_dynamic(): string {
  return UNLOADED_DYNAMIC_CNS;
}

<<__NEVER_INLINE>>
function read_concrete(): int {
  return UNLOADED_CONCRETE_CNS;
}

<<__EntryPoint>>
function main(): void {
  for ($i = 0; $i < 3; $i++) {
    var_dump(basename(read_dynamic()));
    var_dump(read_concrete());
  }
}
