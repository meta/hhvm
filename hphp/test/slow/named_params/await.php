<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>
async function gen(): Awaitable<string> { return 'ok'; }
function consume(named string $value): void { echo $value."\n"; }
async function caller(): Awaitable<void> { consume(value=await gen()); }
<<__EntryPoint>>
function main(): void { \HH\Asio\join(caller()); }
//
