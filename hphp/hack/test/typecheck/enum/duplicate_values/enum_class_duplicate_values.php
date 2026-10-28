<?hh
<<file:__EnableUnstableFeatures('class_type')>>

final class Target {}

enum class Pointers: class<Target> {
  class<Target> FIRST = Target::class;
  class<Target> SECOND = Target::class;
}

enum class Strings: string {
  string FIRST = 'same';
  string SECOND = 'same';
}
