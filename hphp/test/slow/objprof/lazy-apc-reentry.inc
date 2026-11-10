<?hh

class LazyReentryChild {
  public int $value = 'bad default';
}

class LazyReentryOuter {
  public mixed $child = null;
}

class LazyReentryHolder {
  public static mixed $outer = null;
}

class LazyReentryState {
  public static bool $profiling = false;
  public static int $warnings = 0;
}

class LazyReentryScope {
  public static function makeClosure(): (function(): int) {
    return () ==> 42;
  }
}

function handle_lazy_reentry_warning(
  int $errno,
  string $message,
  string $file,
  int $line,
): bool {
  invariant(
    strpos($message, 'LazyReentryChild::value') !== false,
    'Unexpected warning: %s',
    $message,
  );
  if (LazyReentryState::$profiling) {
    ++LazyReentryState::$warnings;
    $closure = LazyReentryScope::makeClosure();
    invariant($closure() === 42, 'Closure scope must be usable after reentry');
  }
  return true;
}

function test_lazy_reentry(bool $paths): void {
  set_error_handler(handle_lazy_reentry_warning<>);
  if (!apc_exists('objprof-lazy-reentry')) {
    $child = new LazyReentryChild();
    $child->value = 42;
    $outer = new LazyReentryOuter();
    $outer->child = $child;
    invariant(apc_store('objprof-lazy-reentry', $outer), 'APC store failed');
    // The first fetch promotes the serialized object to a copied APC object.
    $primed = __hhvm_intrinsics\apc_fetch_no_check('objprof-lazy-reentry');
    invariant($primed->child->value === 42, 'Invalid cached child');
    echo "primed\n";
    return;
  }

  LazyReentryState::$profiling = true;
  LazyReentryHolder::$outer =
    __hhvm_intrinsics\apc_fetch_no_check('objprof-lazy-reentry');
  invariant(LazyReentryState::$warnings === 0, 'Fetch must leave the child lazy');

  // Skip both objects in the heap walk; the static-property walk reaches them.
  $excluded = vec['LazyReentryOuter', 'LazyReentryChild'];
  if ($paths) {
    $data = objprof_get_paths(OBJPROF_FLAGS_DEFAULT, $excluded);
  } else {
    $data = objprof_get_data_extended(OBJPROF_FLAGS_DEFAULT, $excluded);
  }
  invariant(LazyReentryState::$warnings === 1, 'Expected class-init warning');
  invariant(
    LazyReentryHolder::$outer->child->value === 42,
    'Profiling must preserve the cached child value',
  );
  LazyReentryState::$profiling = false;
  echo "profiled\n";
}
