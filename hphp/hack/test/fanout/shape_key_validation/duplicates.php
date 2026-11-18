//// key.php
<?hh

class Key {
  <<__ShapeKey>>
  const arraykey FIELD = 'first';
  <<__ShapeKey>>
  const arraykey OTHER = 'second';
}

//// alias.php
<?hh

type Fields = shape(Key::FIELD => int, Key::OTHER => int);

//// literal.php
<?hh

function literal(): void {
  $_ = shape(Key::FIELD => 1, Key::OTHER => 2);
}

//// method.php
<?hh

class Consumer {
  public function fields(): void {
    $_ = shape(Key::FIELD => 1, Key::OTHER => 2);
  }
}

//// constant.php
<?hh

const shape(Key::FIELD => int, Key::OTHER => int) FIELDS =
  shape(Key::FIELD => 1, Key::OTHER => 2);

//////////////////////

//// key.php
<?hh

class Key {
  <<__ShapeKey>>
  const arraykey FIELD = 'second';
  <<__ShapeKey>>
  const arraykey OTHER = 'second';
}

//////////////////////

//// key.php
<?hh

class Key {
  <<__ShapeKey>>
  const arraykey FIELD = 'first';
  <<__ShapeKey>>
  const arraykey OTHER = 'second';
}
