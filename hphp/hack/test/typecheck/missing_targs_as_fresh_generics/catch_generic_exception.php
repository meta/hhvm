<?hh

final class PayloadException<T> extends Exception {
  public function __construct(public T $payload) {
    parent::__construct();
  }
}

// The catch can't know which T was thrown.
function catch_generic(): int {
  try {
    throw new PayloadException<string>('foo');
  } catch (PayloadException $e) {
    return $e->payload;
  }
}

function catch_generic_mixed(): mixed {
  try {
    throw new PayloadException<string>('foo');
  } catch (PayloadException $e) {
    return $e->payload;
  }
}

final class BoundedException<T as arraykey> extends Exception {
  public function __construct(public T $payload) {
    parent::__construct();
  }
}

// The fresh type argument keeps the declared bound.
function catch_bounded(): arraykey {
  try {
    throw new BoundedException(1);
  } catch (BoundedException $e) {
    return $e->payload;
  }
}

function catch_plain(): string {
  try {
    throw new Exception('foo');
  } catch (Exception $e) {
    return $e->getMessage();
  }
}
