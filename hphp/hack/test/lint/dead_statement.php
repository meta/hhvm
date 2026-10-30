<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

final class TestUnreachableCode {
  public function testContinue(): void {
    for($i = 0; $i < 100; $i++) {
      continue;
      echo 'O Romeo, Romeo! wherefore art thou Romeo?';
    }
  }

  public function testReturn(): string {
    echo 'If music be the food of love, play on,'.
      'Give me excess of it; that surfeiting,'.
      'The appetite may sicken, and so die.';
    return 'He said, in a low tremulous voice...';
    echo 'This is never reached, the farthest distance in the world.';
  }

  public function testYield(string $input): Generator<int, string, void> {
    yield break;
    yield 'is when I have laptop, but no Wifi connection'; // unreached
  }

  public function testSwitch(int $i): string {
    switch ($i) {
    case 1:
        $t = 'hello';
        return 'WoRlD iN LaTeX';
        // FALLTHROUGH -- this is a Hack annotation
     default:
        $t = 'i is unknown';
        break;
    }
    return $t;
  }
}

function unreachable_function(): string {
  return "I had a lover's quarrel with the world";
}

function test_unreachable_function(): string {
  return "Remember: all I'm offering is the truth. Nothing more.";
  echo unreachable_function();
}

function test_unreachable_if(): string {
  if (false) {
    return "Unreachable";
  }
  else {
    return "OK";
  }
}

function test_unreachable_else(): string {
  if (true) {
    return "OK";
  }
  else {
    return "Unreachable";
  }
}

function test_unreachable_if_with_nop(): string {
  if (false) { // Lint warning
  }
  else {
    return "OK";
  }
}

function test_unreachable_else_with_nop(): string {
  if (true) {
    return "OK";
  }
  else { // Lint warning
  }
}

function test_unreachable_after_if_true(): string {
  if (true) {
    return "OK";
  }
  return "Unreachable";
}

function test_unreachable_after_join_1(): string {
  if (HH\Lib\PseudoRandom\int(0,10) === 1) {
    echo "Option 1";
  } else {
    return "Option 2";
  }
  return "OK";
}

function test_unreachable_after_join_2(): string {
  if (HH\Lib\PseudoRandom\int(0,10) === 1) {
    return "Option 1";
  } else {
    echo "Option 2";
  }
  return "OK";
}

function test_unreachable_after_join_3(): string {
  if (HH\Lib\PseudoRandom\int(0,10) === 1) {
    return "Option 1";
  } else {
    return "Option 2";
  }
  return "Unreachable";
}

function test_unreachable_echo_1(): string {
  if (false) {
    echo "Unreachable";
  }
  return "OK";
}

function test_unreachable_echo_2(): string {
  if (true) {
    echo "OK";
  }
  return "OK";
}

function test_unreachable_if_not_true(): string {
  if (!true) {
    return "Unreachable";
  }
  else {
    return "OK";
  }
}

function test_unreachable_else_not_false(): string {
  if (!false) {
    return "OK";
  }
  else {
    return "Unreachable";
  }
}

function test_unreachable_if_not_not_false(): string {
  if (!!false) {
    return "Unreachable";
  }
  return "OK";
}

function test_unreachable_if_not_or(): string {
  if (!(true || HH\Lib\PseudoRandom\int(0,10) === 1)) {
    return "Unreachable";
  }
  return "OK";
}

function test_reachable_if_not_call(): string {
  if (!(HH\Lib\PseudoRandom\int(0,10) === 1)) {
    return "Option 1";
  }
  return "Option 2";
}
