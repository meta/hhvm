<?hh

abstract class Statics {
  abstract const type T;
  private static ?this $option = null;
  private static vec<this> $nested = vec[];
  private static ?this::T $access = null;
  private static ?(function(this): void) $param = null;
  private static ?shape(this...) $splat = null;
}

final class FinalStatics {
  private static ?this $final = null;
}

trait StaticsTrait {
  private static ?this $in_trait = null;
}

class UsesStaticsTrait {
  use StaticsTrait;
}
