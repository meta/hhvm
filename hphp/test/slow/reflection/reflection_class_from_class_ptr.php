<?hh // strict
<<file:__EnableUnstableFeatures('class_type')>>

trait T {}

interface I {}

class C {
  use T;
}

<<__EntryPoint>>
function main_reflection_class_from_class_ptr(): void {
  $rc = new ReflectionClass(C::class);
  echo $rc->getName()."\n";
  var_dump($rc->isTrait());

  $rt = new ReflectionClass(T::class);
  echo $rt->getName()."\n";
  var_dump($rt->isTrait());

  $ri = new ReflectionClass(I::class);
  echo $ri->getName()."\n";
  var_dump($ri->isInterface());

  try {
    $_undef = new ReflectionClass(UndefinedClass::class);
    echo "no exception\n";
  } catch (ReflectionException $e) {
    echo $e->getMessage()."\n";
  }
}
