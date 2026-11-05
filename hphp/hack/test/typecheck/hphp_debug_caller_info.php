<?hh

function takes_class(class<mixed> $c): void {}

function caller_info_fields(): void {
  $info = hphp_debug_caller_info();
  hh_show($info);
  $class = Shapes::idx($info, 'class');
  if ($class is nonnull) {
    takes_class($class);
    takes_class(HH\classname_to_class($class));
  }
}
