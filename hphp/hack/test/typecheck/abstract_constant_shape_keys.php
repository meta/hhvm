<?hh

interface MagicFactory {
  <<__ShapeKey>>
  abstract const string MAGIC;

  public function getShape(): shape(self::MAGIC => string);
}

final class MyFactory implements MagicFactory {
  const string MAGIC = 'magic';

  public function getShape(): shape(MagicFactory::MAGIC => string) {
    return shape(MagicFactory::MAGIC => 'magic');
  }
}

abstract class AbstractMagicFactory {
  <<__ShapeKey>>
  abstract const string MAGIC;

  public function getShapeViaSelf(): shape(self::MAGIC => string) {
    return shape(self::MAGIC => 'magic');
  }
}

final class ConcreteMagicFactory extends AbstractMagicFactory {
  const string MAGIC = 'magic';

  public function getShapeViaBaseName(): shape(
    AbstractMagicFactory::MAGIC => string,
  ) {
    return shape(AbstractMagicFactory::MAGIC => 'magic');
  }
}

trait MagicFactoryTrait {
  <<__ShapeKey>>
  abstract const string MAGIC;

  public function getShapeViaSelf(): shape(self::MAGIC => string) {
    return shape(self::MAGIC => 'magic');
  }
}

final class TraitMagicFactory {
  use MagicFactoryTrait;

  const string MAGIC = 'magic';

  public function getShapeViaTraitName(): shape(
    MagicFactoryTrait::MAGIC => string,
  ) {
    return shape(MagicFactoryTrait::MAGIC => 'magic');
  }
}
