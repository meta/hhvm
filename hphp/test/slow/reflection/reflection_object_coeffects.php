<?hh // strict

class Widget {
  public int $size = 3;
}

class Exporter {
  public function nameViaWriteProps(Widget $w)[write_this_props]: string {
    return (new ReflectionObject($w))->getName();
  }

  public function nameViaPure(Widget $w)[]: string {
    return (new ReflectionObject($w))->getName();
  }
}

<<__EntryPoint>>
function main_reflection_object_coeffects(): void {
  $e = new Exporter();
  echo $e->nameViaWriteProps(new Widget())."\n";
  echo $e->nameViaPure(new Widget())."\n";
}
