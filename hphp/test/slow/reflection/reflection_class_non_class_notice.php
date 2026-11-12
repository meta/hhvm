<?hh // strict
<<file:__EnableUnstableFeatures('class_type')>>

class C {}

<<__EntryPoint>>
function main_reflection_class_non_class_notice(): void {
  set_error_handler((int $_errno, string $msg) ==> {
    if (strpos($msg, 'non-class argument') !== false) {
      echo "NOTICE: ".$msg."\n";
      return true;
    }
    return false;
  });

  $fromString = new ReflectionClass('C');
  echo $fromString->getName()."\n";

  $fromObject = new ReflectionClass(new C());
  echo $fromObject->getName()."\n";

  $fromClass = new ReflectionClass(C::class);
  echo $fromClass->getName()."\n";

  $fromReflectionObject = new ReflectionObject(new C());
  echo $fromReflectionObject->getName()."\n";

  echo "done\n";
}
