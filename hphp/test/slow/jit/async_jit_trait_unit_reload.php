<?hh

// An async JIT region job for a trait method imported into a class from
// another file must not run after the trait's Unit is freed. Unit::destroy()
// only zombie-marks the trait Unit's own funcs and the methods of classes
// defined in it, so the imported method (and the closure created inside it)
// stays a live job target while pointing into the freed Unit.
//
// Each --count iteration is a separate request:
//  - Even iterations call a fresh batch of filler functions so the single
//    async JIT worker falls behind, and only then call the imported trait
//    method and its closure, so their jobs sit at the back of the queue.
//  - Odd iterations only rewrite and reload the trait. The old trait Unit is
//    freed once this short request ends, while the jobs enqueued by the
//    previous request are still waiting behind the fillers.
//  - The last iteration waits for the worker to drain the queue.

const int kIters = 13;
const int kFillers = 500;

function write_file(string $path, string $contents, int $mtime): void {
  file_put_contents($path, $contents);
  touch($path, $mtime);
}

function trait_source(int $version): string {
  return <<<EOT
<?hh

trait T {
  public function m(int \$n): string {
    \$s = '';
    for (\$i = 0; \$i < \$n; \$i++) {
      \$s .= 'trait-method-$version-'.\$i.';';
    }
    return \$s;
  }

  public function c(): (function(int): string) {
    return (int \$x) ==> 'trait-closure-$version-'.\$x.'-'.static::class;
  }
}
EOT;
}

function filler_source(int $iter): string {
  $src = "<?hh\n\n";
  for ($i = 0; $i < kFillers; $i++) {
    $src .= "<<__DynamicallyCallable>>\n";
    $src .= "function filler_{$iter}_{$i}(int \$x): string {\n";
    $src .= "  \$s = 'filler-$iter-$i';\n";
    for ($j = 0; $j < 40; $j++) {
      $src .= "  if (\$x % ".($j + 2)." === 0) { \$s .= 'a$j'; } ".
        "else { \$s .= (string)(\$x * $j); }\n";
    }
    $src .= "  return \$s;\n}\n\n";
  }
  return $src;
}

<<__EntryPoint>>
function main(): void {
  $iter = (int)__hhvm_intrinsics\apc_fetch_no_check('iter');
  apc_store('iter', $iter + 1);

  $dir = sys_get_temp_dir().'/async_jit_trait_unit_reload_'.getmypid();
  $trait_file = $dir.'/trait.php';
  $user_file = $dir.'/user.php';
  if ($iter === 0) {
    if (!is_dir($dir)) mkdir($dir);
    write_file($user_file, "<?hh\n\nclass C { use T; }\n", 1000000);
    write_file($trait_file, trait_source(0), 1000000);
  }

  if ($iter === kIters - 1) {
    sleep(5);
    echo "drained\n";
    return;
  }

  if ($iter % 2 === 1) {
    write_file($trait_file, trait_source($iter), 1000000 + $iter);
    require $trait_file;
    echo "iter $iter: reloaded trait\n";
    return;
  }

  require $trait_file;
  require $user_file;

  $filler_file = $dir."/filler_$iter.php";
  write_file($filler_file, filler_source($iter), 1000000);
  require $filler_file;
  $len = 0;
  for ($i = 0; $i < kFillers; $i++) {
    $len += strlen(HH\dynamic_fun("filler_{$iter}_{$i}")($i));
  }

  $o = new C();
  $m = $o->m(3);
  $c = ($o->c())(7);
  echo "iter $iter: $len $m $c\n";
}
