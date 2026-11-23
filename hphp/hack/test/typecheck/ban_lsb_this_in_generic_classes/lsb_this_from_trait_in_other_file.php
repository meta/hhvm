//// trait.php
<?hh

trait TSlotElsewhere {
  <<__LSB>> private static ?this $slot = null;
}

//// class.php
<?hh

class UsesSlotElsewhere<T> {
  use TSlotElsewhere;
}
