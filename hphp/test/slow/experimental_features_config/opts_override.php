<?hh
// test_feature overridden to Preview via .config
<<file:__EnableUnstableFeatures('test_feature')>>
<<__EntryPoint>>
function main(): void {
  echo __EXPERIMENTAL_TEST_FEATURE_STATUS__ . "\n";
}
