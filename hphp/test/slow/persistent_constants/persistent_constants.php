<?hh

// Exercises Eval.ConstantPersistence. Under Eval.ForceAllPersistent, top-level
// constants with a concrete value are promoted to persistent RDS in
// Constant::def, while dynamic constants (whose value is computed by running
// Hack, so their stored value is KindOfUninit) are left non-persistent. Both
// must still resolve to the same values. See the .config file. A debug (dbgo)
// build additionally checks the RDS handle-mode invariants in Constant::def via
// assertx, so a mis-promoted constant would crash rather than silently pass.

// --- Concrete values: eligible for persistence -----------------------------
const int C_INT = 42;
const string C_STR = "hello";
const float C_FLOAT = 1.5;
const bool C_BOOL = true;
const C_NULL = null;
const vec<int> C_VEC = vec[1, 2, 3];
const dict<string, int> C_DICT = dict['a' => 1, 'b' => 2];
const keyset<string> C_KEYSET = keyset['x', 'y'];
const int C_FOLDED = 3 * 4 + 1; // constant-folded to a concrete 13

// --- Dynamic values: NOT eligible (value computed at runtime via an 86cinit) -
// A top-level const initializer must be a constant expression, but one that
// references other constants is still resolved at runtime (its stored value is
// KindOfUninit), so it exercises the non-promotion path.
const int C_BASE_A = 100;
const int C_BASE_B = 23;
const int C_DYNAMIC = C_BASE_A + C_BASE_B; // references constants -> dynamic
const int C_REFS_OTHER = C_INT + 1; // references a constant -> dynamic

function use_constants(): int {
  // Referenced from a hot function so it gets JITed; under ForceAllPersistent
  // the persistent constants can be baked directly into the translation.
  $sum = C_INT;
  $sum += C_FLOAT > 1.0 ? 1 : 0;
  $sum += C_BOOL ? 1 : 0;
  $sum += C_NULL === null ? 1 : 0;
  $sum += C_VEC[0] + C_VEC[1] + C_VEC[2];
  $sum += C_DICT['a'] + C_DICT['b'];
  $sum += C_FOLDED;
  $sum += C_DYNAMIC;
  $sum += C_REFS_OTHER;
  return $sum;
}

<<__EntryPoint>>
function main(): void {
  $last = 0;
  for ($i = 0; $i < 500; $i++) {
    $last = use_constants();
  }
  echo "checksum=".$last."\n";
  echo "C_INT=".C_INT."\n";
  echo "C_STR=".C_STR."\n";
  echo "C_FLOAT=".(string)C_FLOAT."\n";
  echo "C_BOOL=".(C_BOOL ? "true" : "false")."\n";
  echo "C_NULL_is_null=".(C_NULL === null ? "yes" : "no")."\n";
  echo "C_VEC=".implode(",", C_VEC)."\n";
  echo "C_DICT=".C_DICT['a'].",".C_DICT['b']."\n";
  $ks = vec[];
  foreach (C_KEYSET as $k) {
    $ks[] = $k;
  }
  echo "C_KEYSET=".implode(",", $ks)."\n";
  echo "C_FOLDED=".C_FOLDED."\n";
  echo "C_DYNAMIC=".C_DYNAMIC."\n";
  echo "C_REFS_OTHER=".C_REFS_OTHER."\n";
  echo "done\n";
}
