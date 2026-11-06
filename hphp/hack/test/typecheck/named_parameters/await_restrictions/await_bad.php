<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>
async function gen(): Awaitable<string> { return 'ok'; }
function consume(named string $value): void {}
async function caller(): Awaitable<void> { consume(value=await gen()); }
