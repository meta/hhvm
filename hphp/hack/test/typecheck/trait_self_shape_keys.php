<?hh

trait SelfKey {
  <<__ShapeKey>>
  const string KEY = 'key';

  public function make(): shape(self::KEY => int) {
    return shape(self::KEY => 1);
  }

  public function read(shape(SelfKey::KEY => int) $s): int {
    return $s[self::KEY];
  }

  public function write(shape(SelfKey::KEY => int) $s): void {
    $s[self::KEY] = 2;
  }

  public function dict_read(dict<string, int> $d): ?int {
    return $d[self::KEY] ?? null;
  }

  public function class_name(shape(...) $s): mixed {
    return $s[self::class];
  }
}
