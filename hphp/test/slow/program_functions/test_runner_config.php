<?hh


<<__EntryPoint>>
function main_test_runner_config() :mixed{
var_dump(ini_get("hhvm.env_variables"));
var_dump(ini_get("hhvm.error_handling.notice_frequency"));
var_dump(ini_get("hhvm.error_handling.warning_frequency"));
var_dump(ini_get("hhvm.allow_hhas"));
var_dump(ini_get("hhvm.enable_xhp"));
var_dump(ini_get("hhvm.jit_mature_size"));
var_dump(ini_get("hhvm.http.slow_query_threshold"));
var_dump(ini_get("hhvm.resource_limit.serialization_size_limit"));
var_dump(ini_get("hhvm.server_variables"));
}
