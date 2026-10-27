<?hh

// Sandbox-mode async-JIT test.
//
// Runs with the async JIT enabled (Eval.EnableAsyncJIT) plus the new
// Eval.AsyncJitWaitForTranslate flag, so each translation is produced off the
// request thread and then waited on and run as though async JITing were
// disabled. Eval.ForceAllPersistent is also enabled. See the .config file.
//
// The symbols exercised below each live in a separate file. This file does NOT
// include them -- they are located through the native autoloader / facts, which
// is the way code is resolved in a dev sandbox. The .hhvmconfig.hdf file marks
// this directory as the repo root so native autoloading + facts are enabled.

<<__EntryPoint>>
function main(): void {
  // 1. Function defined in def_function.inc. Called in a loop so the function
  //    is JITed (its translation enqueued on, waited for, and run from the
  //    async JIT worker).
  echo "sandbox_add resolved from: ".
    HH\Facts\function_to_path('sandbox_add')."\n";
  $sum = 0;
  for ($i = 0; $i < 200; $i++) {
    $sum = sandbox_add($sum, $i);
  }
  echo "sandbox_add(0..199) = ".$sum."\n";

  // 2. Class defined in def_class.inc -- instantiated here.
  echo "SandboxWidget resolved from: ".
    HH\Facts\type_to_path('SandboxWidget')."\n";
  $w = new SandboxWidget();
  echo "SandboxWidget->label() = ".$w->label()."\n";

  // 3. Top-level constant defined in def_constant.inc.
  echo "SANDBOX_GREETING resolved from: ".
    HH\Facts\constant_to_path('SANDBOX_GREETING')."\n";
  echo "SANDBOX_GREETING = ".SANDBOX_GREETING."\n";

  // 4. Class constant defined in def_class_constant.inc -- referenced, not
  //    instantiated.
  echo "SandboxLimits resolved from: ".
    HH\Facts\type_to_path('SandboxLimits')."\n";
  echo "SandboxLimits::MAX = ".SandboxLimits::MAX."\n";
}
