<?hh

<<__EntryPoint>>
function main_tier_overrides_tier() :mixed{
var_dump(ini_get("hhvm.log.level"));
var_dump(ini_get("hhvm.server.upload.upload_max_file_size"));
var_dump(ini_get("hhvm.jit_mature_size"));
var_dump(ini_get("hhvm.server.thread_count"));
var_dump(ini_get("hhvm.server.request_timeout_seconds"));
var_dump(ini_get("hhvm.server.enable_keep_alive"));
var_dump(ini_get("hhvm.bogus.setting"));
}
