<?hh

class Widget {
  public int $declared = 1;
  public static string $stat = 's';
}

<<__EntryPoint>>
function main_reflection_object_init(): void {
  $o = new Widget();
  $o->dyn = 'd';

  $ro = new ReflectionObject($o);
  echo $ro->getName()."\n";
  var_dump($ro->hasProperty('dyn'));
  var_dump($ro->hasProperty('declared'));
  echo $ro->getProperty('dyn')->getValue($o)."\n";

  $names = vec[];
  foreach ($ro->getProperties() as $prop) {
    $names[] = $prop->getName();
  }
  sort(inout $names);
  echo implode(',', $names)."\n";

  var_dump($ro->getStaticProperties());
  var_dump(strpos($ro->__toString(), 'Widget') !== false);
}
