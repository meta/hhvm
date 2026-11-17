
namespace HH\__Private\MiniTest;

use type HH\__Private\MiniTest\HackTest;

use namespace HH\Lib\{C, Dict, Keyset, Str, Vec};

<<__EntryPoint>>
async function main(): Awaitable<void> {
  if (!\HH\Facts\enabled()) {
    \fwrite(\HH\stderr(), "ext_facts is required\n");
    exit(1);
  }
  if (\ini_get('hhvm.hsl_systemlib_enabled')) {
    \fwrite(\HH\stderr(), "HSL Systemlib must be disabled\n");
    exit(1);
  }
  if (!\function_exists("HH\\Lib\\Vec\\map")) {
    \fwrite(
      \HH\stderr(),
      "Native autoloader must be configured; couldn't find HSL\n",
    );
    exit(1);
  }

  if (!\class_exists(nameof HackTest)) {
    \fprintf(
      \HH\stderr(),
      "Native autoloader must be configured; couldn't find %s\n",
      nameof HackTest,
    );
    exit(1);
  }

  $test_classes = \HH\Facts\subtypes(
    HackTest::class,
    shape(
      'derive_kind' => keyset[\HH\Facts\DeriveKind::K_EXTENDS],
    ),
  );

  // Usage: minitest [FooTest] [tests/Foo.php]
  $argv = Keyset\map(\HH\global_get('argv') as vec<_>, $val ==> (string)$val)
    |> Keyset\drop($$, 1);
  if (!C\is_empty($argv)) {
    $test_classes = Vec\filter(
      $test_classes,
      $class ==> {
        $path = \HH\Facts\type_to_path($class) as nonnull;
        foreach ($argv as $arg) {
          if (Str\contains($class, $arg)) {
            return true;
          }
          if (Str\contains($path, $arg)) {
            return true;
          }
        }
        return false;
      },
    );
  }

  if (C\is_empty($test_classes)) {
    \fwrite(\HH\stderr(), "No tests selected.\n");
    exit(1);
  }

  foreach ($test_classes as $class) {
    $rc = new \ReflectionClass(\HH\classname_to_class($class));
    if (!$rc->isInstantiable()) {
      continue;
    }
    $c = $rc->getConstructor();
    if ($c !== null && $c->getNumberOfRequiredParameters() !== 0) {
      \printf(
        "!!! class %s has constructor with required parameters\n",
        $class,
      );
    }

    \printf("Starting %s...\n", $class);
    /* HH_FIXME[4128] Minitest intentionally uses reflection to instantiate test classes */
    $obj = $rc->newInstance() as HackTest;
    await $obj->runAsync();
  }
}
