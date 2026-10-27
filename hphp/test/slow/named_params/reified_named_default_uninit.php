<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

// A reified function with a hinted named parameter that has a default, called
// without that argument, does not get the default filled in once optimized:
// the slot is left uninit and the parameter type check on entry fails with
// "must be an instance of string, undefined variable given".  The interpreter
// and the first JIT tier both return the default.
//
// This is the same family as D120690953, which fixed the opposite direction:
// there an explicitly passed null was replaced by the default; here the default
// never arrives.
//
// The .config is what makes this reproducible.  The bug is in f0's optimized
// prologue, and the JIT only builds one when it cannot bind the call directly;
// JitEnableRenameFunction forces that.  Without it a trace shows TransProfile
// and TransOptimize body translations and zero prologues of any kind, and the
// program prints "ok" in every hphp/test/run mode -- which is why this looked
// unreproducible outside the fuzzer at first.  With it, f0 gets 26 `@0p`
// translations and the failure appears in plain mode, no --retranslate-all
// needed.
//
// What lands in the parameter is not always uninit.  Under --retranslate-all it
// reports "undefined variable given"; in plain mode it is "HH\vec given" -- a
// live value from unrelated code.  The prologue is leaving whatever occupies
// the slot rather than filling the default in.
//
// Every ingredient was checked by substitution.  All four are required:
// <reify T>, the named modifier, a hint on the parameter, and omitting the
// argument at the call site.  A positional default is fine, passing the
// argument is fine, and dropping the hint makes the whole divergence go away
// rather than change its shape.

function f0<reify T>(named string $a0 = "x"): string {
  return $a0;
}

<<__EntryPoint>>
function main(): void {
  $bad = 0;
  for ($i = 0; $i < 1000; ++$i) {
    try {
      if (f0<string>() !== "x") {
        $bad++;
      }
    } catch (\Throwable $_) {
      $bad++;
    }
  }
  echo $bad === 0 ? "ok\n" : "bad: $bad\n";
}
