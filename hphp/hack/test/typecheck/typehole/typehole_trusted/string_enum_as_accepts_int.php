<?hh

enum EStr: string as string {
  NUM = '1';
  A = 'a';
}

function to_string(mixed $m): string {
  return $m as EStr;
}

<<__EntryPoint>>
function main(): void {
  to_string(1);
}
