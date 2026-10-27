<?hh
// Regression coverage for the tail-call (musttail) interpreter dispatcher.
//
// The .config file forces -vEval.InterpTailCallDispatch=1 -vEval.Jit=0, so this
// test always runs through dispatchTailCall() (when the toolchain supports
// musttail; otherwise the flag is a harmless no-op and the computed-goto path
// runs, still producing identical output). Eval.InterpTailCallDispatch defaults
// to false, so the rest of the suite exercises the computed-goto path -- this
// file keeps the tail-call path from silently rotting, and its fixed .expect
// asserts the two paths agree.
//
// It deliberately exercises a wide range of opcode families: arithmetic /
// bitwise / comparison binops, string concat, vec/dict/keyset literals and
// member ops, closures, free / method / static calls, inout, switch, foreach /
// for / while / do-while, try / catch / finally (control-flow + exceptions),
// null-coalescing, ternary, instanceof, casts, and varargs.

class Box {
  public function __construct(private int $v)[] {}
  public function get()[]: int { return $this->v; }
  public function bump(int $by)[write_props]: void { $this->v += $by; }
  public static function make(int $v)[]: Box { return new Box($v); }
}

function bump_inout(inout int $x, int $by): void { $x += $by; }

function sum_varargs(int ...$xs): int {
  $s = 0;
  foreach ($xs as $x) { $s += $x; }
  return $s;
}

function classify(int $n): string {
  switch ($n % 3) {
    case 0: return "zero";
    case 1: return "one";
    default: return "two";
  }
}

<<__EntryPoint>>
function main(): void {
  // Arithmetic / bitwise / comparison, dispatch-heavy loop.
  $acc = 0;
  for ($i = 0; $i < 50; $i++) {
    $acc += $i * 3;
    $acc -= 1;
    $acc ^= ($i & 5);
    $acc |= ($i & 2);
    if ($acc > 100000) { $acc = 0; }
  }
  echo "arith=$acc\n";

  // String concat + interpolation.
  $s = "";
  foreach (vec["a", "b", "c"] as $c) { $s .= $c.$c; }
  echo "concat=$s len=".strlen($s)."\n";

  // vec / dict / keyset.
  $v = vec[3, 1, 2];
  usort(inout $v, ($a, $b) ==> $a <=> $b);
  $d = dict["x" => 1, "y" => 2];
  $d["z"] = ($d["x"] ?? 0) + ($d["q"] ?? 9);
  $k = keyset[1, 2, 2, 3];
  echo "vec=".implode(",", $v)." dict_z=".$d["z"]." keyset=".count($k)."\n";

  // Closures capturing by value.
  $mul = 4;
  $f = (int $x) ==> $x * $mul;
  echo "closure=".$f(5)."\n";

  // Free / method / static calls + inout.
  $b = Box::make(10);
  $b->bump(7);
  $n = 100;
  bump_inout(inout $n, 23);
  echo "box=".$b->get()." inout=$n varargs=".sum_varargs(1, 2, 3, 4)."\n";

  // do-while + null coalescing + ternary + instanceof + cast.
  $j = 0; $t = 0;
  do { $t += $j; $j++; } while ($j < 5);
  $maybe = null;
  $val = $maybe ?? 42;
  $obj = $b;
  $isbox = ($obj is Box) ? "yes" : "no";
  echo "dowhile=$t coalesce=$val isbox=$isbox cast=".((int)"77" + 1)."\n";

  // try / catch / finally control flow.
  $log = vec[];
  foreach (vec[0, 1] as $z) {
    try {
      if ($z === 1) { throw new Exception("boom"); }
      $log[] = "ok$z";
    } catch (Exception $e) {
      $log[] = "caught:".$e->getMessage();
    } finally {
      $log[] = "fin$z";
    }
  }
  echo "exc=".implode(",", $log)."\n";

  // switch via helper.
  $cls = vec[];
  for ($i = 0; $i < 6; $i++) { $cls[] = classify($i); }
  echo "switch=".implode(",", $cls)."\n";
}
