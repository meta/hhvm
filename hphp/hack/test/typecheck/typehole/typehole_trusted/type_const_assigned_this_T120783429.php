<?hh

class Bad<T> {
  const type Tthis = this;
  public static function breakit(): TypeStructure<T> {
    return type_structure(Bad::class, 'Tthis')['generic_types'][0];
  }
}

<<__EntryPoint>>
function main():void {
  Bad::breakit();
}
