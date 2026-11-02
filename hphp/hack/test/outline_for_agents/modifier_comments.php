<?hh

module facebook.test;

class ModifierComments {
  final /* private */ public function shownMethod(): void {}
  public /* private */ static int $shownProp = 0;
  protected /* private */ static int $protectedProp = 0;
  protected /* private */ static function protectedMethod(): void {}
  /* private */ public function shownLeadingComment(): void {}
  private /* public */ static function hiddenMethod(): void {}
  private static int $hiddenProp = 0;
  const int COUNT = 0;
  const type T = int;
}

internal /* abstract */ final class NotAbstract {}
internal abstract class ReallyAbstract {}
final class OnlyFinal {}
