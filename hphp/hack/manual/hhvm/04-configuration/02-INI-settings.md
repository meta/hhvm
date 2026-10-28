# INI Settings

Here is the raw list of all possible ini settings that can go in your `/etc/hhvm/php.ini`, `/etc/hhvm/server.ini` or any custom `.ini` file. Not all of them are useful to the HHVM end user. There is lots of cleanup work to do here, but for now you get sorted lists.

Any setting prefixed with `hhvm.` are HHVM-specific options.

## Supported PHP INI Settings

Here is the supported list of [PHP INI settings](http://php.net/manual/en/ini.list.php) for HHVM. The documentation for each is available when you click the option.

In addition to the PHP settings below, you can use `curl.namedPools` to initialize named connection pools via a comma-separated list.

@@ guides-generated-markdown/php_ini_support_in_hhvm.md @@

## Common Options

These are the options that are probably the most commonly used on a day-to-day basis by users of HHVM.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.server_variables` | `array` | `$_SERVER` | Sets the contents of the `$_SERVER` variable.
| `hhvm.enable_obj_destruct_call` | `bool` | `false` | If `false`, `__destruct()` methods will not be called on an object at the end of the request. This can be a performance benefit if your system and application can handle the memory requirements. Deallocation can occur all at one time. If `true`, then HHVM will run all `__destruct()` methods in the usual way.
| `hhvm.hack.lang.look_for_typechecker` | `bool` | `true` | When `true`, HHVM will only process Hack `<?hh` files if the Hack typechecker server is available and running. You normally turn this off in production and it will be turned off automatically in [repo authoritative mode](/hhvm/advanced-usage/repo-authoritative).
| hhvm.jit | `bool` | `true` | Enables the [JIT](https://en.wikipedia.org/wiki/Just-in-time_compilation) [compiler](http://hhvm.com/blog/2027/faster-and-cheaper-the-evolution-of-the-hhvm-jit). This is turned on by default for all supported distributions. Times when you might want to turn this off is for a [short running script](/hhvm/faq#why-is-my-code-slow-at-startup) that may not make use of the JIT.
| `hhvm.jit_enable_rename_function` | `bool` | `false` | If `false`, `fb_rename_function()` will throw a fatal error. And HHVM knowing that functions cannot be renamed can increase performance.
| `hhvm.server.thread_count` | `int` | 2x the number of CPU cores | This specifies the number of worker threads used to serve web traffic in [server mode](/hhvm/basic-usage/server). The number to set here is really quite experimental. If you use [`async`](/hack/asynchronous-operations/introduction), then this number can be the default. Otherwise, you might want a higher number.
| `hhvm.server.source_root` | `string` | working directory of HHVM process | For [server mode](/hhvm/basic-usage/server), this will hold the path to the root of the directory of the code being served up. This setting is *useless* in [repo-authoritative mode](/hhvm/advanced-usage/repo-authoritative).
| `hhvm.force_hh` | `bool` | `false` | If `true`, all code is treated as Hack code, even if it starts with `<?php`.  This setting affects `hhvm.enable_xhp` by forcing it to be `true` as well. This setting affects `hhvm.hack.lang.ints_overflows_to_ints` and `hhvm.log.always_log_unhandled_exceptions` by being the default value for them when they is not explicitly set. This setting affects `hhvm.server.allow_duplicate_cookies` by being the opposite value for a default when it is not explicitly set.
| `hhvm.log.file` | `string` | standard error (`stderr`) | The location of the HHVM error log file. If `hhvm.log.use_cronolog` is set, then this setting will be used as the cron output file.
| `hhvm.repo.authoritative` | `boolean` | `false` | If `true`, you are specifying that you will be using HHVM's [repo-authoritative mode](/hhvm/advanced-usage/repo-authoritative) to serve requests.
| `hhvm.repo.path` | `string` | `""` | The path to the `hhvm.hhbc` file created when you compiled a repo-authoritative repo. (before HHVM 4.115: `hhvm.repo.central.path`)
| `hhvm.server.type` | `string` | `"Proxygen"` | The type of server you want to serve up requests for the HHVM server. The default is `"proxygen"`, but you can also specify `"fastcgi"`.
| `hhvm.server.port` | `int` | `80` | The port on which the HHVM server will listen for requests.
| `hhvm.server.default_document` | `string` | `"index.php"` | The default document that will be served if a page is not explicitly specified.
| `hhvm.server.error_document404` | `string` | `"index.php"` | The default 404 error document that will be served when a 404 error occurs.
| `hhvm.server.user` | `string` | `` | The user to run the HHVM server under. Otherwise, the current user is used.

## PHP 7 Settings

For changes from PHP 5 to PHP 7 which are backwards incompatible, INI options are available to choose which behavior HHVM respects. (Features which are not backwards incompatible are always available.)

The vast majority of users will want to just set `hhvm.php7.all = 1` to fully enable PHP 7 mode and can ignore the rest of the options in this section. They are available primarily for advanced users who want to do a more gradual migration or otherwise track down compatibility issues.

| Setting | Type | Default | Description | PHP 7 RFC
|---------|------|---------|-------------|----------
| `hhvm.php7.all` | `bool` | `false` | Default value for all of the below | N/A
| `hhvm.php7.deprecate_old_style_ctors` | `bool` | `hhvm.php7.all` | Disallow and warn when using old PHP 4 constructors | [Remove PHP 4 constructors](https://wiki.php.net/rfc/remove_php4_constructors)
| `hhvm.php7.engine_exceptions` | `bool` | `hhvm.php7.all` | Enable throwing the new `Error` hierarchy of exceptions | [Engine exceptions](https://wiki.php.net/rfc/engine_exceptions_for_php7)
| `hhvm.php7.int_semantics` | `bool` | `hhvm.php7.all` | Change some edge-case int and float behavior, such as divide/mod by zero | [Integer semantics](https://wiki.php.net/rfc/integer_semantics) with some changes due to [engine exceptions](https://wiki.php.net/rfc/engine_exceptions_for_php7)
| `hhvm.php7.ltr_assign` | `bool` | `hhvm.php7.all` | Make order of assignment in `list()` lvalues consistent | [Abstract syntax tree](https://wiki.php.net/rfc/abstract_syntax_tree)
| `hhvm.php7.no_hex_numerics` | `bool` |`hhvm.php7.all` | Don't consider hex strings to be numeric | [Remove hex support in numeric strings](https://wiki.php.net/rfc/remove_hex_support_in_numeric_strings)
| `hhvm.php7.scalar_types` | `bool` | `hhvm.php7.all` | Enable PHP 7-style scalar type annotations (NB: not the same as Hack's) | [Scalar type declarations](https://wiki.php.net/rfc/scalar_type_hints_v5)
| `hhvm.php7.uvs` | `bool` | `hhvm.php7.all` |  Fix some odd precedence and order of evaluation issues | [Uniform variable syntax](https://wiki.php.net/rfc/uniform_variable_syntax)

## Server Mode

These are settings that are available to you when running HHVM in [server mode](/hhvm/basic-usage/server). In normal, everyday usage, you will not use many of these settings. They are described here for completeness, and in case you might ever need them.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.env_variables` | `Map` | *empty* | Set the contents of the `$_ENV` variable. You set them in the form of `hhvm.env_variables[X]=Y`. If you are setting just one, command line `-d` is fine. Otherwise, for multiple settings, use a `.ini` file.
| `hhvm.http.default_timeout` | `int` | 30 | HTTP default timeout, in seconds.
| `hhvm.http.slow_query_threshold` | `int` | 5000 | If a query takes longer than this setting, it is logged as a slow query.
| `hhvm.php_file.extensions` | `Map<string>` | *empty* | Normally, `.php` and `.hh` are treated as source code. With this setting you can add other file extensions to be treated as source code. You set them in the form of `hhvm.php_file.extensions["pp"]=".pp"`. If you are setting just one, command line `-d` is fine. Otherwise, for multiple settings, use a `.ini` file.
| `hhvm.pid_file` | `string` | `www.pid` | The location of the server process id file.
| `hhvm.ip_block_map`| `Object` | *empty* | A set of IP information that will be blocked or allowed per endpoint. The format for this setting is [below](#ip-block-map-format). You need a `location` and at least one `allow` or `deny`. **This setting is currently not retrievable via `ini_get()`**. This is not normally set.
| `hhvm.satellites` | `Object` | *empty* | If you have various ports for different types of servers, such as an RPC server. The format for this setting is [below](#satellite-format). You need at least a `type` and a `port`. **This setting is currently not retrievable via `ini_get()`**. This is not normally set.
| `hhvm.server_variables` | `Map<string>` | *empty* | Set the contents of the `$_SERVER` variable. You set them in the form of `hhvm.server_variables[X]=Y`. If you are setting just one, command line `-d` is fine. Otherwise, for multiple settings, use a `.ini` file.
| `hhvm.server.allow_duplicate_cookies` | `bool` | `!hhvm.force_hh` | If enabled, this allows duplicate cookies for a name-domain-path triplet.
| `hhvm.server.allowed_exec_cmds` | `Vector<string>` | *empty* | A whitelist of acceptable process commands for something like `pcntl_exec`. This setting is only effective if `hhvm.server.whitelist_exec` is `true`. You set them in the form of `hhvm.server.allowed_exec_cmds[]="cmd"`. If you are setting just one, command line `-d` is fine. Otherwise, for multiple settings, use a `.ini` file.
| `hhvm.server.always_populate_raw_post_data` | `bool` | `false` | Generally, if the content type is multipart/form-data, `$HTTP_RAW_POST_DATA` should not always be available. If this is enabled, then that data will always be available.
| `hhvm.server.always_use_relative_path` | `bool` | `false` | If enabled, files will be looked up and invoked via a relative path. In [sandbox](#sandbox) mode, files always use a relative path.
| `hhvm.server.backlog` | `int` | 128 | The maximum queue length for incoming connections.
| `hhvm.server.connection_limit` | `int` | `0` | The maximum number of connections the server (e.g., [Proxygen](/hhvm/basic-usage/proxygen)) can accept. The default is `0`, which is unlimited.
| `hhvm.server.connection_timeout_seconds` | `int` | `-1` | The maximum number of seconds a connection is allowed to stand idle after its previous read or write. If `-1`, this defaults to the server default (e.g., for Proxygen](/hhvm/basic-usage/proxygen) this is 50 seconds).
| `hhvm.server.dangling_wait` | `int` | `0` | The number of seconds to wait for a dangling server to respond. A [dangling server](https://github.com/facebook/hhvm/blob/master/hphp/doc/server.dangling_server) allows the possibility for an older version of a server to run on a different port in case a page needs to be served from that old version.
| `hhvm.server.default_charset_name` | `string` | `''` | This is used for PHP responses in case no other charset has been set explicitly. `"UTF-8"` is an example of a possible setting.
| `hhvm.server.default_document` | `string` | `"index.php"` | The default document that will be served if a page is not explicitly specified.
| `hhvm.server.default_server_name_suffix` | `string` | `''` | If a server name is not specified for a virtual host, then the virtual prefix is prepended to this setting to create a server name.
| `hhvm.server.enable_early_flush` | `bool` | `true` | Allows chunked encoding responses.
| `hhvm.server.enable_keep_alive` | `bool` | `true` | If enabled, the server will remain open for connection until `hhvm.server.connection_timeout_seconds` timeout.
| `hhvm.server.enable_on_demand_uncompress` | `bool` | `true` | If enabled, this allows on-demand uncompress when reading from the file cache to avoid storing the uncompressed contents for compressible files.
| `hhvm.server.enable_output_buffering` | `bool` | `false` | Turn output buffering on. While output buffering is active no output is sent from the script (other than headers), instead the output is stored in an internal buffer.
| `hhvm.server.enable_ssl` | `bool` | `false` | If enabled, HHVM will allow SSL connections to come through. Related to `hhvm.server.ssl_port`, `hhvm.server.ssl_certificate_file`, `hhvm.server.ssl_certificate_key_file`, `hhvm.server.ssl_certificate_dir`.
| `hhvm.server.enable_static_content_from_disk` | `bool` | `true` | A static content cache creates one single file from all static contents, including css, js, html, images and any other non-PHP files. Normally this is prepared by the compiler at compilation time, but it can also be prepared at run-time, if `hhvm.server.source_root` points to real file directory and this setting is `true`. Otherwise, use `hhvm.server.file_cache` to point to the static content cache file created by the compiler.
| `hhvm.server.enable_static_content_m_map` | `bool` | `true` | This setting being `true` overrides whatever is set for `hhvm.server.enable_on_demand_uncompress`.
| `hhvm.server.error_document404` | `string` | `''` | The default 404 error document that will be served when a 404 error occurs.
| `hhvm.server.error_document500` | `string` | `''` | The default 500 error document that will be served when a 500 error occurs.
| `hhvm.server.evil_shutdown` | `bool` | `true` | Kill anything listening on the server port. This is enabled by default. When stopping a server, HHVM first tries to gracefully shut it down. If that doesn't work, and `hhvm.server.harsh_shutdown` is enabled, it will try to kill the `pid` file. If that doesn't work, then `hhvm.server.evil_shutdown` is invoked.
| `hhvm.server.exit_on_bind_fail` | `bool` | `false` | If the HHVM server cannot start because there is an older HHVM server running, if this flag is enabled, then the current HHVM just quits trying to start the server. If this is not enabled, HHVM try harsher measures to stop the older server so the current one can start up again.
| `hhvm.server.expires_active` | `bool` | `true` | If enabled, then cached static content will expire after the duration of `hhvm.server.expires_default`.
| `hhvm.server.expires_default` | `int` | 2592000 | If `hhvm.server.expires_active` is enabled, then cached static content will expire after this amount of time. The default is `2592000` seconds (or 30 days).
| `hhvm.server.expose_hphp` | `bool` | `true` | Expose the `X-Powered-By HHVM` version header.
| `hhvm.server.expose_xfb_debug` | `bool` | `false` | If enabled, Facebook specific debugging information is exposed.
| `hhvm.server.expose_xfb_server` | `bool` | `false` | Facebook specific debugging information is exposed.
| `hhvm.server.fatal_error_message` | `string` | `''` | If this string is not empty, then when you encounter a 500, the message associated with the string is shown.
| `hhvm.server.file_cache` | `string` | `''` | An absolute path to where the static content (e.g., css, html, etc.) created during compilation should be loaded. `hhvm.server.enable_static_content_from_disk` needs to be enabled for this setting to take effect.
| `hhvm.server.file_socket` | `string` | `''` | If this string is not empty, then a file socket is used instead of an IP address for the server.
| `hhvm.server.fix_path_info` | `bool` | `false` | If enabled, this changes [fastcgi](/hhvm/advanced-usage/fastCGI) path from `SCRIPT_FILENAME` to `PATH_TRANSLATED`.
| `hhvm.server.forbidden_as404` | `bool` | `false` | If the extension of a URI is in the ``hhvm.server.forbidden_file_extensions` map, and this option is enabled, then that extension cannot be used as a 404 option either.
| `hhvm.server.forbidden_file_extensions` | `Set<string>` | *empty* | Map of filename extensions that will not be loaded by the server. You set them in the form of `hhvm.server.forbidden_file_extensions[]=".exe"`. If you are setting just one, command line `-d` is fine. Otherwise, for multiple settings, use a `.ini` file.
| `hhvm.server.force_chunked_encoding` | `bool` | `false` | If enabled, the server will only send chunked encoding responses for uncompressed payloads.
| `hhvm.server.force_compression.cookie` | `string` | `''` | For compression, if this string is set and the cookie is present in the request, then compression should happen.
| `hhvm.server.force_compression.param` | `string` | `''` | For compression, if this string is set and the parameter is present in the request, then compression should happen.
| `hhvm.server.force_compression.url` | `string` | `''` | For compression, if this URL is set and the request matches exactly, then compression has to happen.
| `hhvm.server.force_server_name_to_header` | `bool` | `false` | If enabled, then `$_SERVER['SERVER_NAME']` must come from the request header.
| `hhvm.server.global_document` | `string` | `''` | If specified, the document that handles every URI.
| `hhvm.server.graceful_shutdown_wait` | `int` | `0` | The amount of time to wait for a graceful shutdown of a server. If it doesn't shutdown during that period of time, then `hhvm.server.harsh_shutdown` may be invoked.
| `hhvm.server.gzip_compression_level` | `int` | `3` | When compression with gzip, this is the level of compression that will be used. `1` is fastest. `9` is best.
| `hhvm.server.harsh_shutdown` | `bool` | `true` | When stopping a server, HHVM first tries to gracefully shutdown any previous incarnation of the server. If that doesn't work, and `hhvm.server.harsh_shutdown` is enabled, it will try to kill the `pid` file associated with the server process.
| `hhvm.server.high_priority_end_points` | `Set<string>` | *empty* | A list of http endpoints that will be given request priority. The form is `/endpoint`.
| `hhvm.server.host` | `string` | `''` | The default host for the server.
| `hhvm.server.http_safe_mode` | `bool` | `false` | If enabled, then you cannot open an HTTP stream.
| `hhvm.server.image_memory_max_bytes` | `int` | `0` | The maximum memory size for image process. If `0`, then it will be set to `hhvm.server.upload.upload_max_file_size` * 2.
| `hhvm.server.implicit_flush` | `bool` | `false` | If set to true, then the output buffer will be set to implicitly flush when executing requests.
| `hhvm.server.ip` | `string` | `''`| The default ip address for the server.
| `hhvm.server.kill_on_sigterm` | `bool` | `false` | If enabled, and the server receives a SIGTERM signal, then server will be stopped.
| `hhvm.server.light_process_count` | `int` | `0` | The number of light processes to turn on. Light processes have very little forking cost because they are pre-forked. They can provide for faster shell command execution.
| `hhvm.server.light_process_file_prefix` | `string` | `./lightprocess` | The file prefix for a light process.
| `hhvm.server.lock_code_memory` | `bool` | `false` | Unless this is enabled, during paging the server into memory, the binary is `munlock()`ed.
| `hhvm.server.max_array_chain`| `int` | `INT_MAX` | For balancing arrays. Normally this is best left as the default.
| `hhvm.server.max_post_size` | `int` | `100` | The maximum POST content-length. This is 100 MB.
| `hhvm.server.memory_head_room` | `int` | `0` | How much memory headroom is allowed. If kept at the default, then the active memory limit is `std::numeric_limits<size_t>::max()`.
| `hhvm.server.output_handler` | `string` | `''` | A custom output buffer handler. If left empty, then the default is used.
| `hhvm.server.path_debug`| `bool` | `false` | If a 404 is returned, and this is enabled, then the URL paths examined will be displayed.
| `hhvm.server.port` | `int` | `80` | The port on which the HHVM server will listen for requests.
| `hhvm.server.psp_cpu_timeout_seconds` | `int` | `0` | The length of CPU time a post-send processing request (PSP) is allocated. If `0`, the timer is reset to the last one. If negative, the timeout is set to that value if there is less than that value remaining. If positive, the timeout is set to that value.
| `hhvm.server.psp_timeout_seconds` | `int` | `0` | The length of time a post-send processing request (PSP) is allocated. If `0`, the timer is reset to the last one. If negative, the timeout is set to that value if there is less than that value remaining. If positive, the timeout is set to that value.
| `hhvm.server.request_body_read_limit` | `int` | `-1` | Only valid for a [proxygen](/hhvm/basic-usage/proxygen) server, if set to something other than `-1`, a limit is set on the request buffer and new data won't be added to the buffer until that part of the request is processed.
| `hhvm.server.request_init_document` | `string` | `''` | The document that is loaded and executed upon the server receiving a request.
| `hhvm.server.request_init_function` | `string` | `''` | The function that is executed upon the server receiving a request. If both `hhvm.server.request_init_function` and this is specified, the former is executed first.
| `hhvm.server.request_memory_max_bytes` | `int` | `0` | The maximum number of bytes for a request. If not explicitly set (i.e., remains the default of `0`), this value is set to 16GB or the system's memory limit, which ever is less.
| `hhvm.server.request_timeout_seconds` | `int` | `0` | The amount of time provided for a request to process before the server times out. If `0` (default), there is no explicit request timeout.
| `hhvm.server.safe_file_access` | `bool` | `false` | If enabled, then when a file is accessed it ensures that the file is in an allowed directory, is not an absolute path, and is resolvable.
| `hhvm.server.shutdown_listen_wait` | `int` | `0` | When the [proxygen](/hhvm/basic-usage/proxygen) server is stopped, if this value is set to something positive, then it will continue to listen that number of seconds before actually shutting down.
| `hhvm.server.ssl_certificate_dir` | `string` | `''` | The directory where your SSL certificate information is located. `hhvm.server.enable_ssl` must be enabled for this to take effect.
| `hhvm.server.ssl_certificate_file` | `string` | `''` | The file that contains your SSL certificate. `hhvm.server.enable_ssl` must be enabled for this to take effect.
| `hhvm.server.ssl_certificate_key_file` | `string` | `''` | The file that contains your SSL certificate key. `hhvm.server.enable_ssl` must be enabled for this to take effect.
| `hhvm.server.ssl_port` | `int` | `443` | The port for SSL connections. `hhvm.server.enable_ssl` must be enabled for this to take effect.
| `hhvm.server.stat_cache` | `bool` | `false` | If enabled, the server will cache calls to [stat()](https://en.wikipedia.org/wiki/Stat_(system_call)).
| `hhvm.server.takeover_filename` | `string` | `''` | Between server instances port takeover, this is the file that is used.
| `hhvm.server.thread_count` | `int` | 2x the number of CPU cores | This specifies the number of worker threads used to serve web traffic in [server mode](/hhvm/basic-usage/server). The number to set here is really quite experimental. If you use [`async`](/hack/asynchronous-operations/introduction), then this number can be the default. Otherwise, you might want a higher number.
| `hhvm.server.thread_drop_cache_timeout_seconds` | `int` | `0` | The amount of time for a server thread to drop its cache.
| `hhvm.server.thread_drop_stack` | `bool` | `false` | If we have timed out via `hhvm.server.thread_drop_cache_timeout_seconds`, if this is enabled, then we also flush the thread stack itself.
| `hhvm.server.thread_job_lifo_switch_threshold` | `int` | `INT_MAX` | An option where the request processing order can flip between FIFO or LIFO based on the length of the queue. If the job queue is configured to be in FIFO mode, and the current queue length exceeds lifoSwitchThreshold, then the workers will begin work on requests in LIFO order until the queue size is below the threshold in which case we resume in FIFO order.
| `hhvm.server.thread_job_max_queuing_milli_seconds` | `int` | `-1` | If set to a positive number, this will be the number of milliseconds that will be waited for a job before it expires.
| `hhvm.server.thread_round_robin` | `bool` | `false` | By default, the last thread serves next. If this setting is enabled, then serving is done round robin style.
| `hhvm.server.tls_client_cipher_spec` | `string` | `''` | If not empty, then the [SSL ciphers](https://www.openssl.org/manmaster/ssl/SSL_CTX_set_cipher_list.html) are set with the control string provided.
| `hhvm.server.tls_disable_tls1_2` | `bool` | `false` | If enabled, then the [TLSv1.2 protocol](https://www.openssl.org/manmaster/ssl/SSL_CTX_set_options.html) is disabled.
| `hhvm.server.type` | `string` | `"proxygen"` | The type of server you want to serve up requests for the HHVM server. The default is `"proxygen"`, but you can also specify `"fastcgi"`.
| `hhvm.server.unserialization_whitelist_check` | `bool` | `false` | If enabled, only classes in the whitelist passed as the second argument to the `unserialize()` method will be allowed to be unserialized.
| `hhvm.server.unserialization_whitelist_check_warning_only` | `bool` | `true` | If `hhvm.server.unserialization_whitelist_check` is enabled, then if a class is attempted to be unserialized that is not in the whitelist, and this is enabled, only a warning will be displayed instead of erroring.
| `hhvm.server.upload.enable_file_uploads` | `bool` | `true` | Allow files to be uploaded.
| `hhvm.server.upload.enable_upload_progress` | `bool` | `false` | If enabled, then a file upload progress bar can be displayed. For example, the client can then query the apc user cache to find out the upload progress and then display an upload progress bar.
| `hhvm.server.upload.rfc1867freq` | `int` | `256 * 1024` (256K) | If `hhvm.server.upload.enable_upload_progress` is enabled, then this is how often the progress will be updated.
| `hhvm.server.upload.rfc1867name` | `string` | `video_ptoken` | The hidden form entry name that activates APC upload progress and specifies the user cache key suffix.
| `hhvm.server.upload.rfc1867prefix` | `string` | `vupload_` | Key prefix for the user cache entry generated by rfc1867 upload progress functionality.
| `hhvm.server.upload.upload_max_file_size` | `int` | `100` (MB) | The maximum file upload size.
| `hhvm.server.upload.upload_tmp_dir` | `string` | '/tmp' | The temporary directory used for file uploads.
| `hhvm.server.use_direct_copy` | `bool` | `false` | If enabled, files will be renamed using very little disk-cache.
| `hhvm.server.user` | `string` | `` | The user to run the HHVM server under. Otherwise, the current user is used.
| `hhvm.server.utf8ize_replace` | `bool` | `true` | Whether to convert and replace all characters with UTF8.
| `hhvm.server.warmup_throttle_request_count` | `int` | `0` | If positive, and if `hhvm.server.thread_count` is greater than the number of processors, then the starting number of warmup threads will be the same as `hhvm.server.thread_count` - (the number of processors).
| `hhvm.server.warn_on_collection_to_array` | `bool` | `false` | If enabled, every time you try to convert a collection to an array, you will get a warning explaining that the operation is expensive and should be avoided.
| `hhvm.server.whitelist_exec` | `bool` | `false` | If enabled, then this sets a whitelist of commands that will be executed, given by `hhvm.server.allowed_exec_cmds`.
| `hhvm.server.whitelist_exec_warning_only` | `bool` | `false` | If enabled, and `hhvm.server.whitelist_exec` is enabled, if you try to execute a command outside the whitelist, you will only get a warning.
| `hhvm.server.xfb_debug_ssl_key` | `string` | `bool` | If `hhvm.expose_xfb_debug` or `hhvm.expose_xfb_server` is enabled, this will be the SSL key that will be used.
| `hhvm.static_file.extensions` | `Map<string>` | (see description) | Map of filename extensions to content types for use by the proxygen server. The defaults are [below](#static-file-extension-defaults). You set them in the form of `hhvm.static_file.extensions[]=Y`. If you are setting just one, command line `-d` is fine. Otherwise, for multiple settings, use a `.ini` file.
| `hhvm.static_file.files_match` | `Vector<string>` | *empty* | A list of file extensions that will have http transport headers added to them. The format of this setting is [below](#static-file-files-match-format). You need a `pattern` and at least one `header`. **This setting is currently not retrievable via `ini_get()`**. This is not normally set.
| `hhvm.static_file.generators` | `Set<string>` | *empty* | Dynamic files that serve up static content. This is not normally set. You set them in the form of `hhvm.static_file.generators[]="/path/to"`. If you are setting just one, command line `-d` is fine. Otherwise, for multiple settings, use a `.ini` file.
| `hhvm.tiers` | `Object` | *empty* | COMING SOON to ini. Allows you to override settings based on various tier settings like CPU, machine name, etc.
| `hhvm.virtual_host`| `Object` | *empty* | You can map server domains as various [virtual hosts](https://en.wikipedia.org/wiki/Virtual_hosting) with a lot of settings described [below](#virtual-host-format). You need at least a `prefix` or a `pattern`. **This setting is currently not retrievable via `ini_get()`**.

### Static File Extension Defaults

These are the default file extensions for `hhvm.static_file.extensions`:

```
array(11) {
  ["zip"]=>
  string(15) "application/zip"
  ["jpeg"]=>
  string(10) "image/jpeg"
  ["html"]=>
  string(9) "text/html"
  ["css"]=>
  string(8) "text/css"
  ["gif"]=>
  string(9) "image/gif"
  ["mp3"]=>
  string(10) "audio/mpeg"
  ["png"]=>
  string(9) "image/png"
  ["tif"]=>
  string(10) "image/tiff"
  ["jpg"]=>
  string(10) "image/jpeg"
  ["tiff"]=>
  string(10) "image/tiff"
  ["txt"]=>
  string(10) "text/plain"
}
```

### Static File Files Match Format

This the ini format for `hhvm.static_file.files_match`:

```
hhvm.static_file.files_match[0][pattern]   = "[regex](here)*"
hhvm.static_file.files_match[0][headers][] = "header1"
hhvm.static_file.files_match[0][headers][] = "header2"
```

### IP Block Map Format

This is an example of the ini format for `hhvm.ip_block_map`.

```
hhvm.ip_block_map[0][location]     = /endpoint
hhvm.ip_block_map[0][allow_first]  = true
hhvm.ip_block_map[0][ip][allow][0] = 127.0.0.1
hhvm.ip_block_map[0][ip][deny][0]  = 8.32.0.0/24
hhvm.ip_block_map[0][ip][deny][1]  = aaaa:bbbb:cccc:dddd:eeee:ffff:1111::/80
```

`allow_first` basically says whether you allow an ip by default or not. You can then have other endpoints with `[1]`, `[2]`, etc.

### Satellite Format

The options you can give for `hhvm.satellites` are:

```
Satellites {
  NAME {
    Type = RPCServer | InternalPageServer | DanglingPageServer

    Port = 0  # disabled
    ThreadCount = 5

    # only for RPCServer
    MaxRequest = 500
    MaxDuration = 120    # in seconds
    TimeoutSeconds = 30  # default to RequestTimeoutSeconds
    RequestInitFunction = on_init
    RequestInitDocument = filename
    Password = authentication
    Passwords {
      * = password
    }
    # only for InternalPageServer
    BlockMainServer = true
    URLs {
      * = pattern
    }
  }
}
```

This is an example of the ini format for `hhvm.satellites`.

```
hhvm.satellites[rpc][type] = RPCServer
hhvm.satellites[rpc][port] = 9999
hhvm.satellites[rpc][request_init_document] = my/rpc/rpc.php
hhvm.satellites[rpc][request_init_function] = init_me
hhvm.satellites[rpc][password] = abcd0987
hhvm.satellites[rpc][passwords][] = abcd0987
hhvm.satellites[ips][type] = InternalPageServer
hhvm.satellites[ips][block_main_server] = false
hhvm.satellites[ips][urls][] = url/here
```

### Virtual Host Format

The options you can give to `hhvm.virtual_host` are:


```
VirtualHost {
  NAME {
    Disabled = false
    Prefix = prefix.
    Pattern = regex pattern
    PathTranslation = html
    CheckExistenceBeforeRewrite = true
    ServerName =
    ServerVariables {
      name = value
    }
    Overwrite = {
      [default][setting] = new value
    }

    RewriteRules {
      * {
        pattern = regex pattern same as Apache's
        to = target format same as Apache's
        qsa = false
        redirect = 0 (default: off) | 302 | 301 | other status code

        conditions {
          * {
            pattern = regex pattern to match
            type = host | request
            negate = false
          }
        }
      }
    }

    IpBlockMap {
      # in same format as the IpBlockMap example above
    }

    # Remove certain query string parameters from access log.
    LogFilters {
      * {
        # regex pattern to match a URL
        url = (empty means matching all URLs)

        # names of parameters to remove from query string
        params = {
          * = parameter name
        }

        # alternatively, use regex pattern to replace with empty string.
        pattern = (empty means hiding entire query string)

        # optionally, specify what values to replace with
        value = (by default it's empty, removing the query parameter)
      }
    }
  }
}
```

This is an example of the ini format for `hhvm.virtual_hosts`:

```
hhvm.server.allowed_directories[] = /var/www
hhvm.server.allowed_directories[] = /usr/bin
hhvm.virtual_host[flibtest][prefix] = my.
hhvm.virtual_host[flibtest][path_translation] = flib/_bin
hhvm.virtual_host[flibtest][server_name] = my.example.org
hhvm.virtual_host[flibtest][log_filters][0][url] = function/searchme
hhvm.virtual_host[flibtest][log_filters][0][params][0] = v
hhvm.virtual_host[flibtest][log_filters][0][params][1] = t
hhvm.virtual_host[flibtest][log_filters][0][params][2] = btoken
hhvm.virtual_host[flibtest][log_filters][0][params][3] = ptoken
hhvm.virtual_host[flibtest][log_filters][0][value] = INSERTED
hhvm.virtual_host[flibtest][log_filters][1][url] = property/searchme
hhvm.virtual_host[flibtest][log_filters][1][pattern] = #thePattern#
hhvm.virtual_host[flibtest][log_filters][1][value] = BETWEEN
hhvm.virtual_host[upload][prefix] = upload.
hhvm.virtual_host[upload][server_variables][TFBENV] = 8
hhvm.virtual_host[upload][overwrite][server][allowed_directories][0] = /var/www
hhvm.virtual_host[upload][overwrite][server][allowed_directories][1] = /mnt
hhvm.virtual_host[upload][overwrite][server][allowed_directories][2] = /tmp
hhvm.virtual_host[upload][overwrite][server][allowed_directories][3] = /var/tmp/tap
hhvm.virtual_host[upload][overwrite][server][max_post_size] = 100MB
hhvm.virtual_host[upload][overwrite][server][upload][upload_max_file_size] = 100MB
hhvm.virtual_host[upload][overwrite][server][request_timeout_seconds] = 120
hhvm.virtual_host[upload][path_translation] = html
hhvm.virtual_host[default][path_translation] = htm
hhvm.virtual_host[default][log_filters][0][url] = method/searchme
hhvm.virtual_host[default][log_filters][0][params][0] = q
hhvm.virtual_host[default][log_filters][0][params][1] = s
hhvm.virtual_host[default][log_filters][0][params][2] = atoken
hhvm.virtual_host[default][log_filters][0][params][3] = otoken
hhvm.virtual_host[default][log_filters][0][value] = REMOVED
hhvm.virtual_host[default][rewrite_rules][common][pattern] = /html/common/
hhvm.virtual_host[default][rewrite_rules][common][to] = http://example.org
hhvm.virtual_host[default][rewrite_rules][common][qsa] = true
hhvm.virtual_host[default][rewrite_rules][common][redirect] = 301
```

## Feature flags

These settings enable various features in the runtime, including Hack-specific features.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.enable_obj_destruct_call` | `bool` | `false` | If `false`, `__destruct()` methods will not be called on an object at the end of the request. This can be a performance benefit if your system and application can handle the memory requirements. Deallocation can occur all at one time. If `true`, then HHVM will run all `__destruct()` methods in the usual way.
| `hhvm.force_hh` | `bool` | `false` | If `true`, all code is treated as Hack code, even if it starts with `<?php`.  This setting affects `hhvm.enable_xhp` by forcing it to be `true` as well. This setting affects `hhvm.hack.lang.ints_overflows_to_ints` and `hhvm.log.always_log_unhandled_exceptions` by being the default value for them when they are not explicitly set. This setting affects `hhvm.hack.lang.look_for_typechecker` and `hhvm.server.allow_duplicate_cookies` by being the opposite value for a default when they are not explicitly set.
| `hhvm.hack.lang.ints_overflow_to_ints` | `bool` | Value of `hhvm.force_hh` | Value of `hhvm.force_hh` | Don't check if integer arithmetic might overflow, just use asm intrinsics and do whatever the underlying processor would do, most likely a twos-complement wraparound. If disabled, then check for integer overflow, and promote up to a float if so. (Skipping the check is considerably faster.)
| `hhvm.hack.lang.look_for_typechecker` | `bool` | Opposite value of `hhvm.force_hh` | If enabled, make sure that Hack code is under a directory with a `.hhconfig` file, and error otherwise.
| `hhvm.enable_args_in_backtraces` | `bool` | `true` | If disabled, then arguments are not shown in PHP backtraces.
| `hhvm.enable_asp_tags` | `bool` | `false` | If enabled, then you can use `<% %>`.
| `hhvm.enable_hip_hop_experimental_syntax` | `bool` | `false` | Enables experimental syntax, including type hints for local variables and global variables.
| `hhvm.enable_numa` | `bool` | `false` | Enable [NUMA](https://en.wikipedia.org/wiki/Non-uniform_memory_access) integration.
| `hhvm.enable_numa_local` | `bool` | `false` | This causes all allocations from threads to be allocated on the local node (except for a few that have been explicitly marked interleaved). `hhvm.enable_numa` must be set to `true` for this to take affect.
| `hhvm.enable_short_tags` | `bool` | `false` | If enabled, this allows the `<?` tag.
| `hhvm.enable_xhp` | `bool` | `false` | If `true`, this will enable XHP support in PHP files. (XHP is always enabled in Hack files.) If `hhvm.force_hh` is set to `true`, then this setting is automatically `true`.
| `hhvm.enable_zend_compat` | `bool` | `false` | If `true`, this enable the support layer for Zend PHP extensions that we have directly [migrated to HHVM](https://github.com/facebook/hhvm/tree/master/hphp/runtime/ext_zend_compat) (e.g. FTP).
| `hhvm.authoritative_mode` | `bool` | `false` | If enabled, HHVM disallows constructs that are unavailable in [Repo Authoritative](/hhvm/advanced-usage/repo-authoritative) mode even when you are not in Repo Authoritative mode (i.e., when `hhvm.repo.authoritative` is `false`).

## Logging

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.log.access_log_default_format` | `string` | `%h %l %u %t \"%r\" %>s %b` | The access log format. if not specified, the default uses Apache access log formatting.
| `hhvm.log.admin_log.file` | `string` | `''` |  For the admin server, this will be the location and name for its log file.
| `hhvm.log.admin_log.format` | `string` | `%h %t %s %U` | The admin access log format.
| `hhvm.log.admin_log.sym_link` | `string` | `''` | You can provide a symlink to the `hhvm.log.admin_log.file`.
| `hhvm.log.always_escape_log` | `bool` | `true` | By default, escape characters in the logs are escaped. Disable this to not escape them.
| `hhvm.log.always_log_unhandled_exceptions` | `bool` | `hhvm.force_hh` | Unhandled exceptions are HHVM fatal errors, and AlwaysLogUnhandledExceptions will make sure they get logged even if a user's error handler is installed for them. The default value is equivalent to what you set for `hhvm.force_hh`.
| `hhvm.log.drop_cache_chunk_size` | `int` | `1 << 20` (1 MB) | When dropping the cache, logging will happen after this many bytes have been written.
| `hhvm.log.file` | `string` | standard error (`stderr`) | The location of the HHVM error log file. If `hhvm.log.use_cronolog` is set, then this setting will be used as the cron output file.
| `hhvm.log.force_error_reporting_level` | `int` | `0` | A bitmask that will be ORed (`|`) with `hhvm.log.runtime_error_reporting_level` to determine the actual error reporting level.
| `hhvm.log.header` | `bool` | `false` | If enabled, the request header is logged. Header includes timestamp, process id, 34 thread id, request id (counted from 1 since the server started), message id (counted from 1 since request started) and extra header text from command line option.
| `hhvm.log.header_mangle` | `int` | `0` | This setting controls the logging of potentially malicious headers.  If set to a value greater than 0, then HHVM will log one in every `n` (where `n` is the value of the setting) requests where a header collision has occurred.  Such collisions almost certainly indicate a malicious attempt to set headers which are either set or filtered by a proxy.
| `hhvm.log.level` | `string` | `Warning` | The level of output to the log. From least to most verbose: `None`, `Error`, `Warning`, `Info`, `Verbose`.
| `hhvm.log.max_messages_per_request` | `int` | `-1` | Controls maximum number of messages each request can log. If positive, then that number will be the threshold. Otherwise, unlimited messages per request can be logged.
| `hhvm.log.native_stack_trace` | `bool` | `true` | Turn off to disable the logging of the native stack trace. There are two kinds of stacktraces: (1) C++ stacktrace, which is hex-encoded and printed on every line of logging right after header. These stacktraces can be translated into human readable frames by running "-m translate" with the compiled program. (2) PHP stacktrace from code injection. Generated C++ code injects stacktrace preparation code into every frame of functions and methods.
| `hhvm.log.no_silencer` | `bool` | `false` | If enabled, even when silencer (`@`) operator is used, still output errors.
| `hhvm.log.runtime_error_reporting_level` | `int` | `HPHP_ALL` | A numeric setting similar to [error_reporting](http://php.net/manual/en/errorfunc.constants.php) in PHP. See [below](#error-reporting-levels) for the possible settings.
| `hhvm.log.sym_link` | `string` | `''` | You can provide a symlink to the `hhvm.log.file`. If `hhvm.log.use_cronolog` is set, then this setting will be used to create a symlink to the cron output file.
| `hhvm.log.use_cronolog` | `bool` | `false` | If enabled, this will switch to a rotating log paradigm via [`cronolog`](http://linux.die.net/man/1/cronolog).
| `hhvm.log.use_log_file` | `bool` | `true` | The default logging mechanism via the log file specified by `hhvm.log.file`.
| `hhvm.log.use_request_log` | `bool` | `false` | If enabled, requests are logged into a special file where the root of that file is specified `hhvm.sandbox.logs_root`.
| `hhvm.log.use_syslog` | `bool` | `false` | Also log to the system log (syslog).
| `hhvm.log.access` | `Map<string>` | *empty* | The location and format of explicit access logs. You set them in the form of `hhvm.log.access["file"]="format"`. If you are setting just one, command line `-d` is fine. Otherwise, for multiple settings, use a `.ini` file. **This setting is currently not retrievable via `ini_get()`**

### Error Reporting Levels

Here are the error reporting levels you can provide to `hhvm.log.runtime_error_reporting_level`.

```
ERROR = 1,
WARNING = 2,
PARSE = 4, // not supported
NOTICE = 8,
CORE_ERROR = 16, // not supported
CORE_WARNING = 32, // not supported
COMPILE_ERROR = 64, // not supported
COMPILE_WARNING = 128, // not supported
USER_ERROR = 256,
USER_WARNING = 512,
USER_NOTICE = 1024,
STRICT = 2048,
RECOVERABLE_ERROR = 4096,
PHP_DEPRECATED = 8192, // DEPRECATED conflicts with macro definitions
USER_DEPRECATED = 16384,

/*
 * PHP's fatal errors cannot be fed into error handler. HipHop can. We
 * still need "ERROR" bit, so old PHP error handler can see this error.
 * The extra 24th bit will help people who want to find out if it's
 * a fatal error only HipHop throws or not.
 */
FATAL_ERROR = ERROR | (1 << 24), // 16777217

PHP_ALL = ERROR | WARNING | PARSE | NOTICE | CORE_ERROR | CORE_WARNING |
  COMPILE_ERROR | COMPILE_WARNING | USER_ERROR | USER_WARNING |
  USER_NOTICE | RECOVERABLE_ERROR | PHP_DEPRECATED | USER_DEPRECATED,

HPHP_ALL = PHP_ALL | FATAL_ERROR,

/* Errors that can be upgraded to E_USER_ERROR. */
UPGRADEABLE_ERROR = WARNING | USER_WARNING | NOTICE | USER_NOTICE
```

## Admin Server

The [admin server](/hhvm/advanced-usage/admin-server) allows the administrator of the HHVM server to query and control the HHVM server process.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.admin_server.password` | `string` | `''` | A password required for all requests to the admin server, other than the index. Is passed to the request as the `auth` request var.
| `hhvm.admin_server.port` | `int` | `0` | The port the admin server should listen on. If set to 0, the admin server is not started.
| `hhvm.admin_server.thread_count` | `int` | `1` | The number of threads the admin server should use.

## CLI Server

The [CLI server](/hhvm/advanced-usage/CLI-server) allows you to run command line scripts on a running HHVM server.  This means the translation cache can persist between runs of your scripts.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.use_remote_unix_server` | `string` | `no` | Enable the use of the CLI server.
| `hhvm.unix_server_path` | `string` | `''` | The path to the unix socket for the CLI server.
| `hhvm.unix_server_workers` | `int` | CPU cores | The number of worker threads the CLI server will allow.
| `hhvm.unix_server_quarantine_apc` | `bool` | `false` | Quarantine APC from the scripts being run.
| `hhvm.unix_server_quarantine_units` | `bool` | `false` | Quarantine the loaded units from the scripts being run on a per user bases.
| `hhvm.unix_server_verify_exe_access` | `bool` | `false` | Checks units are readable by client before executing them on the server.
| `hhvm.unix_server_fail_when_busy` | `bool` | `false` | Fail if there are no available workers.
| `hhvm.unix_server_allowed_users` | `Vector<string>` | *empty* | Users allowed to use the CLI server.
| `hhvm.unix_server_allowed_groups` | `Vector<string>` | *empty* | Groups allowed to use the CLI server.

## Debug Settings

These settings are useful when you are debugging actual HHVM issues.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.debug.clear_input_on_success` | `bool` | `true` | Automatically delete requests that had a 200 response code when recording input. This allows for the capturing of error-prone requests without cluttering up with good ones.
| `hhvm.debug.core_dump_email` | `string` | `''` | When HHVM produces a core dump, if this is set to an email address, then an email will be sent with the core dump information.
| `hhvm.debug.core_dump_report` | `bool` | `true` | If enabled, HHVM will generate a core dump report as necessary.
| `hhvm.debug.core_dump_report_directory` | `string` | `/tmp` | When a core dump is produced, it will be put in this directory.
| `hhvm.debug.native_stack_trace` | `bool` | `false` | If enabled, a native static trace will be produced.
| `hhvm.debug.profiler_output_dir` | `string` | `/tmp` | Building with GOOGLE_CPU_PROFILER set lets you collect profiles from the server or from the command line. This option lets you control where the profiles get created.
| `hhvm.debug.record_input` | `bool` | `false` | If enabled, record bad HTTP requests to a file `/tmp/hphp_request_XXXXXX`.
| `hhvm.debug.server_error_message` | `bool` | `false` | If enabled, turn on error messages in HTTP responses with detailed stacktrace information. Send the error message to the output stream - corresponds to `display_errors` in PHP.
| `hhvm.debug.simple_counter.sample_stack_count` | `int` | `0` | HHVM has an implementation of a set of [request-local named counters](https://github.com/facebook/hhvm/commit/e3896255cb329bfe09dee9e0e9f3fa19fcd7abd3). This setting controls how many samples of the named counter should there be.
| `hhvm.debug.simple_counter.sample_stack_depth` | `int` | `5` | For each sample stack from `hhvm.debug.simple_counter.sample_stack_count`, this controls the depth of each sample.
| `hhvm.dump_ast` | `bool` | `false` | If enabled, the AST will be dumped to `stdout`.
| `hhvm.dump_bytecode` | `int` | `0` | If positive, before executing a PHP file, dump out its HHBC along with some interesting metadata, then continue executing the file. If `1` then the user PHP bytecode is dumpted. If `2` then user PHP and System library information is dumped. Useful only for debugging.
| `hhvm.dump_hhas` | `bool` | `false` | If enabled, instead of executing a PHP file, dump out an hhas (HHVM Assembly) file and exit. Useful for debugging, or if you need to directly write some hhas and need an easy way to get started.
| `hhvm.dump_ring_buffer_on_crash` | `int` | `0` | If positive, then when HHVM crashes, the ring buffer will be dumped.
| `hhvm.dump_tc` | `bool` | `false` | If enabled, dump contents of translation cache when executing program from the command line.
| `hhvm.dump_tc_anchors` | `bool` | `false` | If enabled, dump the translation cache anchors.
| `hhvm.keep_perf_pid_map` | `bool` | `false` | If enabled, don't delete the perf pid map.
| `hhvm.perf_data_map` | `bool` | `false` | If enabled, generate a perf data map.
| `hhvm.perf_pid_map` | `bool` | `true` | If enabled, generate a perf pid map.

## Sandbox

A sandbox in HHVM is a set of configuration options (document root, log file path, etc.) that can be used to serve your web application.

Here are a few **important** points:

- The sandbox configuration file must end in `.hdf` or `.hphp`. Most people name it `.hphp`.
- Having a configuration file end in `.ini` is currently broken, but a fix is being worked on now. When HDF is removed in favor of INI, this will be fixed.
- A user is always appended to `hhvm.sandbox.home`. So if you set that setting to `/home`, it will end up being `/home/user`. Thus the `hhvm.sandbox.conf_file` will end up having an absolute path of `/home/user/.hphp`.
- The sandbox pattern assumes that you have valid URLs that can be associated with that pattern. You would need to have those URLs bound in something like `/etc/hosts` (e.g., `127.0.0.1 user-another_site.localhost.com`).
- If you do not specify a sandbox name in the URL, it assumes the default sandbox. e.g., if you type or `curl` `user.localhost.com`, that will assume your default sandbox.
- If you enable `hhvm.sandbox.from_common_root`, make sure you have running code available from that root, or that root prepended by the value of `hhvm.sandbox.directories_root`, if you have that set as well.
- If you are using the HHVM builtin webserver [proxygen](/hhvm/basic-usage/proxygen), as long as you are running the server from a location where there is access to your sandbox (e.g., the root of a sandbox directory), all of your sandboxes URLs should be available to you for testing.

Below is a general configuration setup for a sandbox that you can use as a template.

*ini file*

`user-another_site.localhost.com` would fit the `hhvm.sandbox.pattern` pattern.

```
hhvm.sandbox.sandbox_mode = 1
hhvm.sandbox.home=/home
hhvm.sandbox.conf_file=.hphp
hhvm.sandbox.pattern=([^-]*(-[^-]*)?).localhost.com
```

*Sandbox configuration file ~/.hphp*

If you have `hhvm.sandbox.home set`, `default.xxx` can be relative to that directory (remembering that a `user` is appended to what you set as `hhvm.sandbox.home`). For example, below, if we had `hhvm.sandbox.home = /home`, then we could set `default.path` to `sites/www`.

The `default.ServerVars.ANY_SERVER_VARIABLE=1` is just an example.

```
default.path = /home/user/sites/www
default.log = /home/user/sites//error_log
default.accesslog = /home/user/logs/access_log
default.ServerVars.ANY_SERVER_VARIABLE=1

another_site.path = /home/user/sites/another-site
another_site.log = /home/user/sites/another-site/logs/error_log
another_site.accesslog = /home/user/sites/another-site/logs/access_log
```

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.sandbox.sandbox_mode` | `bool` | `false` | If enabled, sandbox mode is turned on; generally coincides with turning on [HHVM server debugging](#debugger).
| `hhvm.sandbox.home` | `string` | `''` | The home directory of your sandbox. e.g., If this is set to `/home`, then your sandbox path will be something like `/home/joelm/`.
| `hhvm.sandbox.conf_file` | `string` | `''` | The file which contains sandbox information like the path to the default sandbox, the path to other sandboxes, log paths, etc. You can use this file in conjunction with some of some of the other specific sandbox options. For example, if `hhvm.sandbox.home` is set, then this setting is *relative* to that path.
| `hhvm.sandbox.pattern` | `string` | `''` | The URL pattern of your sandbox host. It is a generally a regex pattern with at least one capture group. For example `www.[user]-[sandbox].[machine].yourdomain.com` or `www.([^-]*(-[^-]*)?).yourdomain.com`
| `hhvm.sandbox.from_common_root` | `bool` | `false` | If enabled, your sandboxes will be created from a common root path. This root path is based upon the `hhvm.sandbox.pattern` that you specify and the value of it will be the root string before the first `.` in the pattern. If you have a pattern like `([^-]*(-[^-]*)?).localhost.com` which maps to a sandbox at `user-another_site.localhost.com`, the root that is established by enabling this setting is `/joelm-another_site`. This setting as `true` supersedes any setting you have for `hhvm.sandbox.conf_file`.
| `hhvm.sandbox.directories_root` | `string` | `''` | If you have `hhvm.sandbox.from_common_root` enabled, this value will be prepended to your common root.
| `hhvm.sandbox.logs_root` | `string` | `''` | If you have `hhvm.sandbox.from_common_root` enabled, this value will be prepended to your common root.
| `hhvm.sandbox.fallback` | `string` | `''` | If for some reason your home path in `hhvm.sandbox.home` cannot be accessed, this will be your fallback to set as your home path.
| `hhvm.sandbox.server_variables` | `map` | *empty* | Any server variables that you want accessible when running your sandbox.

## Debugger

A sandbox is commonly used in conjunction with [debugging](#debugger) to debug HHVM in [server mode](/hhvm/basic-usage/server). When you connect the debugger to a server mode process, you will be given the option of a sandbox on which to attach the debugger. The first option you will always see (and attach to by default) is the dummy sandbox, which has no document root. It is primarily used for real-time evaluation of code from the debugger prompt.

These options are used to allow you to use the `hphpd` debugger remotely via a sandbox. HHVM must be running in [server mode](/hhvm/basic-usage/server), as there needs to be a server process on which to attach.

These are the common `.ini` file options to set to enable HHVM to start a debugger in server mode.

```
hhvm.sandbox.sandbox_mode=1
hhvm.sandbox.home=/home
hhvm.sandbox.conf_file=.hphp
hhvm.sandbox.pattern="([^-]*(-[^-]*)?)\.localhost\.com"
hhvm.debugger.enable_debugger = 1
hhvm.debugger.enable_debugger_server = 1
hhvm.debugger.default_sandbox_path = /path/to/your/sandbox
```

If you run your server as `hhvm -m server -c this.ini` and in another terminal, type `hhvm -m debug -h localhost`, you will see:

```
Welcome to HipHop Debugger!
Type "help" or "?" for a complete list of commands.

Connecting to localhost:8089...
Attaching to user's default sandbox and pre-loading, please wait...
localhost> m l
  1 user's default sandbox at /home/user/sites/www/
```

To start debugging:

```
localhost> break start
break start
Breakpoint 1 set start of request
localhost> continue
continue
```

Then you can make a web request via your browser or `curl` to your sandbox URL. You can set breakpoints in your sandbox code to stop at certain places of execution as well.

If you want to debug another sandbox instead of the default, you can explicitly specify the sandbox:

```
hhvm -m debug -h localhost --debug-sandbox another_site
```


| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.debugger.enable_debugger` | `bool` | `false` | You must set this to try in order for HHVM to listen to connections from the debugger.
| `hhvm.debugger.enable_debugger_server` | `bool` | `false` | This option is generally set in conjunction with `hhvm.debugger.enable_debugger` and actually allows for the listening to connections and remote debugging.
| `hhvm.debugger.default_sandbox_path` | `string` | `''` | Path to source files; similar to [`hhvm.server.source_root`](#common-options).
| `hhvm.debugger.disable_ipv6` | `bool` | `false` | If enabled, the debugger will only be able to communicate with ipv4 addresses (AF_INET).
| `hhvm.debugger.enable_debugger_color` | `bool` | `true` | Disable this if you do not want color in the debugger.
| `hhvm.debugger.enable_debugger_prompt` | `bool` | `true` | Disable this if you do not want a debugger prompt to be shown.
| `hhvm.debugger.enable_debugger_usage_log` | `bool` | `false` | *Currently this is only an internal setting*.
| `hhvm.debugger.port` | `int` | `8089` | The port on which debugger clients may connect.
| `hhvm.debugger.rpc.default_auth` | `string` | `''` | The password to be able to debug the HHVM server in RPC mode.
| `hhvm.debugger.rpc.default_port` | `int` | `8083` | The port on which commands will be sent to the server via RPC.
| `hhvm.debugger.rpc.default_timeout` | `int` | `30`| The timeout for commands to be available on the RPC port.
| `hhvm.debugger.rpc.host_domain` | `string` | `''` | The domain where your RPC server is hosted.
| `hhvm.debugger.signal_timeout` | `int` | `1` | The amount of time the debugger waits for a signal from the client before sending to a default dummy sandbox.
| `hhvm.debugger.startup_document` | `string` | `''` | The file that is executed before any other, when the server starts. Does not have to be the same your [default document](#common-options). Similar to `hhvm.server.startup_document`.
| `hhvm.bypass_access_check` | `bool` | `false` | If enabled, forces the debugger bypass access checks on class members.
| `hhvm.script_mode` | `bool` | `false` | If enabled, then the debugger is debugging in script mode.
| `hhvm.tutorial` | `bool` | `false` | If enabled, you can go through the debugger tutorial.
| `hhvm.tutorial.visited` | `bool` | `false` | If enabled, this will tell the debugger you have already seen the tutorial.
| `hhvm.print_level` | `int` | `5` | The amount of printing you want in the debugger.
| `hhvm.source_root` | `string` | `''` | If explicitly set, then the debugger will look for source code there. Otherwise, it is where the debugger is running from.
| `hhvm.small_step` | `bool` | `false` | If enabled, small steps will be used instead of entire lines.
| `hhvm.stack_args` | `bool` | `true` | If disabled, then stack arguments will not be shown in the debugger.
| `hhvm.max_code_lines` | `int` | `-1` | If positive, then limit the code shown after a step to that number of code lines. If `0`, then do not show any code after a step or next. If `-1`, then just show the default number of lines.
| `hhvm.utf8` | `bool` | `true` | If enabled, then use UTF8 by default.
| `hhvm.short_print_char_count` | `int` | `200` | Display at most count characters when doing `= command`.
| `hhvm.macros` | `Map` | *empty* | A custom list of macros that you want to be able to run in the debugger. You set them in the form of `hhvm.macros[name][]="cmd1"`, `hhvm.macros[name][]="cmd2"`, etc.. If you are setting just one, command line `-d` is fine. Otherwise, for multiple settings, use a `.ini` file.
| `hhvm.never_save_config` | `bool` | `false` | If enabled, then your debugger configuration settings will not be saved.

## Error Handling

These settings are used to help set or throttle certain errors/warnings/etc. that may arise in your PHP or Hack code.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.error_handling.call_user_handler_on_fatals` | `bool` | `false` | Call a callback installed via `set_error_handler()` when a fatal error occurs.
| `hhvm.error_handling.max_serialized_string_size` | throw exception if unserializing a string greater than this amount | `64 * 1024 * 1024` (64 MB) |
| `hhvm.error_handling.no_infinite_recursion_detection` | `bool` | `false` | If enabled, do not raise an error if infinite recursion is detected.
| `hhvm.error_handling.notice_frequency` | `int` | `1` | Log every N notices, where N is this number. If the value is `<=0`, then no notices are logged.
| `hhvm.error_handling.throw_exception_on_bad_method_call` | `bool` | `true` | If enabled, throw an error if calling a method on a non-object.
| `hhvm.error_handling.upgrade_level` | `int` | `0` | Bitmask of errors to upgrade to `E_USER_ERROR`. Only `E_WARNING`, `E_USER_WARNING`, `E_NOTICE`, and `E_USER_NOTICE` are supported.
| `hhvm.error_handling.warning_frequency` | `int` | `1` | Log every N warnings, where N is this number. If the value is `<=0`, then no warnings are logged.

## XML

The following are settings that you can use for `libxml` or `simplexml`, as specified.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.libxml.ext_entity_whitelist` | `string` | `'data'` | Loading of external entities in the libxml extension is disabled by default for security reasons. It can be re-enabled on a per-protocol basis with this setting. It is a comma separated list of protocols. The common ones are `file`, `http`, `compress.zlib`. An example of how the setting is set is: `hhvm._libxml.ext_entity.whitelist=file,http`. **This setting is currently not retrievable via `ini_get()`**.
| `hhvm.simple_xml.empty_namespace_matches_all` | `bool` | `false` | If enabled, an empty XML namespace matches all namespaces.

## JIT Settings

These settings can allow fine-grain tuning of HHVM's [JIT](https://en.wikipedia.org/wiki/Just-in-time_compilation) [compiler](http://hhvm.com/blog/2027/faster-and-cheaper-the-evolution-of-the-hhvm-jit).

The [hacker's guide](https://github.com/facebook/hhvm/blob/master/hphp/doc/hackers-guide/jit-core) goes more in depth nin how the JIT works as well.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.jit` | `bool` | `true` | Enables the JIT compiler. This is turned on by default for all supported distributions. Times when you might want to turn this off is for a [short running script](/hhvm/faq#why-is-my-code-slow-at-startup) that may not make use of the JIT.
| `hhvm.jit_always_interp_one` | `bool` | `false` |
| `hhvm.jit_disabled_by_hphpd` | `bool` | `false` | If enabled, the JIT is disabled in the debugger.
| `hhvm.jit_enable_rename_function` | `bool` | `false` | If `false`, `fb_rename_function()` will throw a fatal error. And HHVM knowing that functions cannot be renamed can increase performance.
| `hhvm.jit_global_translation_limit` | `int` | `-1` | How often the JIT does translation before falling back to interpreted code. The default is to always JIT.
| `hhvm.jit_max_request_translation_time` | `int` | `-1` | Sets a maximum number of microseconds to spend per request translating. This is useful for deployments that can't rely on a warm-up script. Default is an unlimited amount of time. Requires `hhvm.jit_timer` to be enabled.
| `hhvm.jit_keep_dbg_files` | `bool` | `false` | If enabled, elf writer based debug files will be kept.
| `hhvm.jit_llvm` | `int` | `0` | If enabled, then use the experimental LLVM backend, see [this blog post](http://hhvm.com/blog/10205/llvm-code-generation-in-hhvm) for more information. If `0`, LLVM is not used. If `1`, LLVM is used for TransOptimize translations. If `2`, LLVM is used for all translations.
| `hhvm.jit_loops` | `bool` | `true` | Disable this if you don't want loops to be compiled by the JIT, but rather interpreted.
| `hhvm.jit_max_translations` | `int` | `12` | Limits the number of translations allowed per `srckey`, and once this limit is hit any further retranslation requests will result in a call out to the interpreter.
| `hhvm.jit_no_gdb` | `bool` | `true` | If set to `false`, the the JIT will be enabled when running HHVM in GDB.
| `hhvm.jit_profile_interp_requests` | `int` | `1` or `11` | Profile interpreted requests per this number of times. For the defaults, if HHVM was compiled in debug mode, then `1`. Otherwise (e.g., release mode) `11`.
| `hhvm.jit_profile_record` | `bool` | `false` | If enabled, HHVM will record a profile of requests.
| `hhvm.jit_profile_requests` | `int` | `2147483648` or `500` | The number of requests to profile. For the defaults, if HHVM was compiled in debug mode, then `2147483648`; otherwise (e.g., release mode) `500`.
| `hhvm.jit_pseudomain` | `bool` | `true` | Whether or not to JIT pseudomains (code that doesn't exist inside a class). This is `false` by default on ARM.
| `hhvm.jit_region_selector` | `string` | `tracelet` | The regions of code that will be translating the bytecode to machine code. The default is is single basic blocks, or tracelets. Other options are `method` and `none`.
| `hhvm.jit_relocation_size` | `int` | `1048576` (1 MB) | The size of code relocation.
| `hhvm.jit_require_write_lease` | `bool` | `false` |  The write Lease guards write access to the translation caches, srcDB, and TransDB. The term "lease" is meant to indicate that the right of ownership is conferred for a long, variable time: often the entire length of a request. If a request is not actively translating, it will perform a "hinted drop" of the lease: the lease is unlocked but all calls to `acquire(false)`` from other threads will fail for a short period of time.
| `hhvm.jit_retranslate_all_request` | `int` | `3000` or `0` | The number of profile requests, before retranslating all code into optimized translations.
| `hhvm.jit_stress_lease` | `bool` | `false` | If HHVM was compiled in debug mode, the you can enable this setting to cede the write lease half the time.
| `hhvm.jit_target_cache_size` | `int` | `67108864` | The size, in bytes, of the target cache.
| `hhvm.jit_timer` | `bool` | `true` | Timer is used to track how much CPU time we spend in the different stages of the jit. Typical usage starts and stops timing with construction/destruction of the object, respectively. The stop() function may be called to stop timing early, in case it's not reasonable to add a new scope just for timing.
| `hhvm.jit_trans_counters` | `bool` | `false` | If enabled, JIT translation profiling counters will be enabled.
| `hhvm.jit_unlikely_dec_ref_percent` | `int` | `10` | The percentage of ref counted types to destroy.
| `hhvm.jit_use_vtune_api` | `bool` | `false` | If enabled, the VTune API will be used for profiling.

### JIT Translation Cache Size

The translation cache stores the JIT'd code. It is allocated as a single region, under 2GB, that is partitioned between the main, cold and frozen sections on demand, so the individual sections have no configured size.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.jit_a_size` | `int` | `62914560` (60 MB) | Target size of the main section, used to decide when the JIT is warmed up. Does not size the translation cache.
| `hhvm.jit_a_hot_size` | `int` | `0` | Size of hot code cache. (Enabled only in [RepoAuthoritative mode](/hhvm/advanced-usage/repo-authoritative) when `hhvm.repo.authoritative` is `true`).
| `hhvm.jit_a_prof_size` | `int` | `67108864` (64 MB) | Size of profiling code cache. (Recommended: 1x `hhvm.jit_a_size`).
| `hhvm.tc_num_huge_cold_mb` | `int` | `4` | The number of megabytes of cold regions to hold in the translation cache.
| `hhvm.tc_num_huge_hot_mb` | `int` | `16` | The number of megabytes of hot regions to hold in the tranlsation cache.

### PGO

These are settings of the JIT for [Profile Guided Optimizations](https://en.wikipedia.org/wiki/Profile-guided_optimization).

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.jit_pgo` | `bool` | `true` | Turn on PGO in the JIT. This is disabled by default for ARM.
| `hhvm.jit_pgo_hot_only` | `bool` | `false` | If enabled, only profile hot sections of code.
| `hhvm.jit_pgo_region_selector` | `string` | `hotcfg` | The regions to translate during PGO. The default are hot sections of code.
| `hhvm.jit_pgo_release_vv_min_percent` | `int` | `10` | The minimum percentage of extra args, varenvs and locals that will be released.
| `hhvm.jit_pgo_threshold` | `int` | `2` or `5000` | The maximum amount of PGO translations. For the defaults, `2` when HHVM is compiled in debug mode; `5000` otherwise.
| `hhvm.jit_pgo_use_post_conditions` | `bool` | `true` | For profiling translations, grab the postconditions to be used for region selection whenever we decide to retranslate.

## Garbage Collector Settings

These are settings for the automated garbage collector.  To enable the automated GC, set `hhvm.enable_gc` to true.

| Setting | Type | Default | Description
| ------- | ---- | ------- | -----------
| `hhvm.enable_gc` | `bool` | `HHVM_EAGER_GC` | Enable the garbage collector.
| `hhvm.eager_gc` | `bool` | `HHVM_EAGER_GC` | Debugging tool. Run the GC as often as possible. Only supported in Debug builds.
| `hhvm.filter_gc_points` | `bool` | `true` | Limit eager gc runs to once per surprise point.
| `hhvm.quarantine` | `bool` | `HHVM_EAGER_GC` | Debugging tool. Instead of freeing memory, mark it as a 'Hole', fill with 0x8a, and leak.
| `hhvm.gc_sample_rate` | `int` | `0` |
| `hhvm.gc_min_trigger` | `int` | `64 << 20` |
| `hhvm.gc_trigger_pct` | `double` | `0.5` |


## APC Settings

These are custom HHVM settings to the [Alternative PHP Cache (APC)](http://php.net/manual/en/book.apc.php).

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.server.apc.expire_on_sets` | `bool` | `false` | If enabled, this will turn on item purging on expiration. And it is done once per `hhvm.server.apc.purge_frequency` of sets.
| `hhvm.server.apc.purge_frequency` | `int` | `4096` | Expired items will be purged every this many APC sets.
| `hhvm.server.apc.purge_rate` | `int` | `-1` | Evict at most this many items on each purge. No limit if `-1`.

## Repo Authoritative

When using HHVM's [Repo-Authoritative](/hhvm/advanced-usage/repo-authoritative) mode, these are the settings that help configure its use.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.repo.authoritative` | `boolean` | `false` | If `true`, you are specifying that you will be using HHVM's repo- authoritative mode to serve requests.
| `hhvm.repo.path` | `string` | `""` | The path to the `hhvm.hhbc` file created when you compiled a repo-authoritative repo. (before HHVM 4.115: `hhvm.repo.central.path`)
| `hhvm.repo.debug_info` | `bool` | `true` | If enabled, the full source locations will be stored in the repo; otherwise, only line numbers will be stored.
| `hhvm.repo.local.path` | `string` | `''` | `hhvm.repo.local.path` or the environment variable `HHVM_REPO_LOCAL_PATH` (the former takes precedence) can be used to specify where the local repo is. If unspecified, then the local repo is `path/to/cli.php.hhbc` in [cli](/hhvm/basic-usage/command-line) mode or `<cwd>/hhvm.hhbc` in [server](/hhvm/basic-usage/server) mode.
| `hhvm.disable_some_repo_auth_notices` | `bool` | `true` | Make the repo authoritative notices you receive less verbose.

## Statistics

These settings allow you to collect various statistics for various parts of the runtime, from web statistics and mysql to memory and the [profiler](https://github.com/facebook/hhvm/blob/master/hphp/doc/profiling).

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.stats.enable` | `bool` | `false` | Set this to `true` in order to enable stats collection.
| `hhvm.stats.apc` | `bool` | `false` | Set this to `true` to enable collection APC statistics.
| `hhvm.stats.enable_hot_profiler` | `bool` | `true` | Enable the hot profiler and [xhprof](http://php.net/manual/en/book.xhprof.php).
| `hhvm.stats.max_slot` | `int` | `72` (12 hours) | For each page, we collect stats by time slots. Each time slot is configured as `hhvm.stats.slot_duration` seconds and server internally keeps the number of slots specified by this setting. Inside each slot, we keep a set of stats by page or URL. These stats include 3 built-in ones ("url", "code" and "hit") and many key-value pairs defined by different parts of the system.
| `hhvm.stats.memory `| `bool` | `false` | Set this to `true` to enable memory statistics.
| `hhvm.stats.network_io` | `bool` | `false` | Set this to `true` to enable network I/O statistics and status.
| `hhvm.stats.profiler_max_trace_buffer` | `int` | `0` | The maximum size of a trace buffer in the profiler; if `0`, there is no maximum.
| `hhvm.stats.profiler_trace_buffer` | `int`| `2000000` | The size of the profiler trace buffer array.
| `hhvm.stats.profiler_trace_expansion` | `double` | `1.2` | How much bigger to make the profiler trace buffer array when it gets full. `hhvm.stats.profiler_trace_buffer` * this setting.
| `hhvm.stats.slot_duration` | `int` | `600` (seconds) | How long each slot described in `hhvm.stats.max_slot` is kept.
| `hhvm.stats.sql` | `bool` | `false` | If set to `true`, this will enable the collection of MySQL connection statistics.
| `hhvm.stats.sql_table` | `bool` | `false` | If set to `true`, this will enable the collection of MySQL table statistics.
| `hhvm.stats.web` | `bool` | `false` | If set to `true`, this will enable the collection of web/server statistics.
| `hhvm.stats.xsl` | `string` | `''` | Stats are written as XML. If this is set to something non-empty, the XSL used for the XML is referred to by that setting.
| `hhvm.stats.xsl_proxy` | `string` | `''` | Stats are written as XML. If this is set to somehing non-empty, the XSL proxy used for the XML is referred to by that setting.

## Resource Limits

These are resource limit settings such as how often to check maximum memory and the default socket timeout.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.resource_limit.core_file_size` | `int` | `0` | If set to something `> 0`, then this is the maximum size of a core dump file. If `0`, the no core dumps are created.
| `hhvm.resource_limit.drop_cache_cycle` | `int` | `0` | If set to something `> 0`, then this is how often the disk cache is dropped.
| `hhvm.resource_limit.max_rss` | `int` | `0` | If set to something `> 0`, then this is the maximum amount of memory (in bytes) of the HHVM process should get.
| `hhvm.resource_limit.max_rss_polling_cycle` | `int` | `0` | If set to something `> 0`, then this is how often to check whether we are hitting our `hhvm.resource_limit.max_rss` limit.
| `hhvm.resource_limit.max_sql_row_count` | `int` | `0` | If set to something `> 0`, then this is the maximum number of rows that will be fetched at any given time.
| `hhvm.resource_limit.serialization_size_limit` | `int` | `2146435072` (~2 GB) | The maximum size of a serialized string.
| `hhvm.resource_limit.socket_default_timeout` | `int` | `60` | The amount of time (in seconds) before an unused socket times out.

## Regular Expressions

These are the settings you can toggle for HHVM's [PCRE](http://www.pcre.org/) implementation.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.preg.backtrace_limit` | `int` | `1000000` | The maximum bind length a call to something like `preg_replace_callback()` can make.
| `hhvm.preg.error_log` | `bool` | `true` | If a PRCE error occurs, then it will be logged if this is enabled.
| `hhvm.preg.recursion_limit` | `int` | `100000` | The maximum recursion limit for PCRE. Setting this value too high could cause the utilization of all of the process stack.
| `hhvm.pcre_cache_type` | `string` | `static` | May be `static`, for a very fast cache which never evicts, `lru`, for a cache which evicts the least-recently used item when full, or `scalable` for a cache which is slightly slower than `lru` at low concurrency but much faster for a high-concurrency tight-loop workload.
| `hhvm.pcre_table_size` | `int` | `0` | The number of patterns which can be stored in the PCRE cache.

## MySQL

These setting control the behavior of the HHVM MySQL extension.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.mysql.typed_results`| `bool` | `true` | Zend returns strings and `null` only for MySQL results, not integers or floats. HHVM return ints (and, sometimes, actual doubles). This behavior can be disabled by setting this to `false`.
| `hhvm.mysql.slow_query_threshold` | `int` | `1000` | In milliseconds, log slow queries as errors.
| `hhvm.mysql.read_only` | `bool` | `false` | If enabled, the database is read only.
| `hhvm.mysql.read_timeout` | `int` | `60000` | How long, in milliseconds, before a read query times out.
| `hhvm.mysql.connect_timeout` | `int` | `1000` | How long, in milliseconds, before a connection times out.
| `hhvm.mysql.wait_timeout` | `int` | `-1` | If positive, how long, in milliseconds, before a wait timeout.
| `hhvm.mysql.kill_on_timeout` | `bool` | `false` | If enabled, when a query takes long time to execute on server, client has a chance to kill it to avoid extra server cost.
| `hhvm.mysql.socket` | `string` | `''` | Default location to look for `mysql.sock`.

## Advanced Settings

These are settings that generally won't be used by most users of HHVM. Some won't be documented, other than the type and default value.

### HHIR

These settings control various aspects of the [HHVM Intermediate Representation](https://github.com/facebook/hhvm/blob/master/hphp/doc/hackers-guide/jit-core#hhir) (HHIR).

| Setting | Type | Default
|---------|------|--------
| `hhvm.hhir_alloc_simd_regs` | `bool` | `true`
| `hhvm.hhir_licm` | `bool` | `false`
| `hhvm.hhir_dead_code_elim` | `bool` | `true`
| `hhvm.hhir_direct_exit` | `bool` | `true`
| `hhvm.hhir_enable_coalescing` | `bool` | `true`
| `hhvm.hhir_enable_gen_time_inlining` | `bool` | `true`
| `hhvm.hhir_enable_pre_coloring` | `bool` | `true`
| `hhvm.hhir_gen_opts` | `bool` | `true`
| `hhvm.hhir_generate_asserts` | `bool` | `true` (if HHVM was compiled in debug mode); `false` otherwise.
| `hhvm.hhir_global_value_numbering` | `bool` | `true`
| `hhvm.hhir_inline_frame_opts` | `bool` | `true`
| `hhvm.hhir_inline_singletons` | `bool` | `true`
| `hhvm.hhir_inline_region_mode` | `string` | `"both"`
| `hhvm.hhir_inlining_max_bind_jmps` | `int` | `0`
| `hhvm.hhir_inlining_max_cost` | `int` | `13`
| `hhvm.hhir_pgo_inlining_max_cost` | `int` | `6`
| `hhvm.hhir_inlining_max_depth` | `int` | `4`
| `hhvm.hhir_inlining_max_return_dec_refs` | `int` | `6`
| `hhvm.hhir_inlining_max_returns` | `int` | `3`
| `hhvm.hhir_memory_opts` | `bool` | `true`
| `hhvm.hhir_outline_generic_inc_dec_ref` | `bool` | `true`
| `hhvm.hhir_prediction_opts` | `bool` | `true`
| `hhvm.hhir_refcount_opts` | `bool` | `true`
| `hhvm.hhir_simplification` | `bool` | `true`
| `hhvm.hhir_store_pre` | `bool` | `true`
| `hhvm.hhir_stress_spill` | `bool` | `true`
| `hhvm.hhir_type_check_hoisting` | `bool` | `false`

### Xbox Server

An xbox server provides cross-machine communication, similar to a message queuing system. It also allows local processing of messages, then working as a multithreading facility for PHP execution. More documentation will be coming for xbox applications.

| Setting | Type | Default
|---------|------|--------
| `hhvm.xbox.default_local_timeout_milli_seconds` | `int` | `500`
| `hhvm.xbox.default_remote_timeout_seconds` | `int` | `5`
| `hhvm.xbox.process_message_func` | `string` | `xbox_process_message`
| `hhvm.xbox.server_info.always_reset` | `bool` | `false`
| `hhvm.xbox.server_info.log_info `| `bool` | `false`
| `hhvm.xbox.server_info.max_duration` | `int` | `120`
| `hhvm.xbox.server_info.max_queue_length` | `int` | `INT_MAX`
| `hhvm.xbox.server_info.max_request` | `int` | `500`
| `hhvm.xbox.server_info.port` | `int` | `0`
| `hhvm.xbox.server_info.request_init_document` | `string` | `''`
| `hhvm.xbox.server_info.request_init_function` | `string` | `''`
| `hhvm.xbox.server_info.thread_count` | `int` | `10`
| `hhvm.xbox.server_info.warmup_document` | `string` | `''`
| `hhvm.xbox.password` | `string` | `''` (not `ini_get()` enabled)
| `hhvm.xbox.passwords` | `Set<string>` | *empty* (not `ini_get()` enabled)

### Pagelet Server

A pagelet server is essentially the same as local CURL, except it's more efficient. This allows parallel execution of a web page, preparing two panels or iframes at the same time.

| Setting | Type | Default
|---------|------|--------
| `hhvm.pagelet_server.queue_limit` | `int` | `0`
| `hhvm.pagelet_server.thread_count` | `int` | `0`
| `hhvm.pagelet_server.thread_drop_cache_timeout_seconds` | `int` | `0`
| `hhvm.pagelet_server.thread_drop_stack` | `bool` | `false`
| `hhvm.pagelet_server.thread_round_robin` | `bool` | `false`

### Emitter

Settings for the HHVM compiler code emitter.

| Setting | Type | Default
|---------|------|--------
| `hhvm.emit_switch` | `bool` | `true`
| `hhvm.enable_emitter_stats` | `bool` | `true`
| `hhvm.random_hot_funcs` | `bool` | `false`

### Xenon

Settings for the Xenon server, which snapshots server activity at regular intervals, showing which code is on CPU.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.xenon.force_always_on` | `bool` | `false` | Gather PHP and async stacks at every function enter/exit. This will gather potentially large amounts of data and is mostly useful for small script debugging.
| `hhvm.xenon.period` | `double` | `0.0` | Configure Xenon to gather PHP and async stacks every this many seconds.

### Mail

These are settings for the Mail extension.

| Setting | Type | Default
|---------|------|--------
| hhvm.mail.force_extra_parameters | `string` | `''`
| hhvm.mail.sendmail_path | `string` | `sendmail -t -i`

### Code Checks

These settings are for toggling various code checks (e.g., typehints).

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.check_flush_on_user_close` | `bool` | `true` | Determines whether or not close() for user streams (registered by `stream_wrapper_register()`) will consider the return value of the implicit call to `flush()`` when calculating its return value.
| `hhvm.check_return_type_hints` | `int` | `2` | Whether to check return type hints at runtime, and if so how. `0` means no return type checking. `1` - Raises `E_WARNING` if a return type hint fails.  `2` - Raises `E_RECOVERABLE_ERROR` if regular return type hint fails, raises `E_WARNING` if soft return type hint fails. If a regular return type hint fails, it's possible for execution to resume normally if the user error handler doesn't throw and returns something other than boolean `false`. `3` - Same as `2`, except if a regular type hint fails the runtime will not allow execution to resume normally; if the user error handler returns something other than boolean `false`, the runtime will throw a fatal error (this goes together with `hhvm.hard_return_type_hints`).
| `hhvm.hard_return_type_hints` | `bool` | `false` | Check hard return type hints. Only used when compiling.
| `hhvm.check_sym_link` | `bool` | `true` | Whether to follow symlinks when looking up units in the bytecode cache.

### Profiling

These are additional settings for the HHVM profiler.

| Setting | Type | Default
|---------|------|--------
| `hhvm.profile_bc` | `bool` | `false`
| `hhvm.profile_heap_across_requests` | `bool` | `false`
| `hhvm.profile_hw_enable` | `bool` | `true`
| `hhvm.profile_hw_events` | `string` | `''`
| `hhvm.record_code_coverage` | `bool`| `false` |
| `hhvm.code_coverage_output_file` | `bool` | `false`
| `hhvm.hot_func_count` | `int` | `4100`
| `hhvm.num_single_jit_requests` | `int` | `5` (in server mode); `0` otherwise

### Proxies

These are settings for reverse proxying.

| Setting | Type | Default
|---------|------|--------
| `hhvm.proxy.origin` | `string` | `''`
| `hhvm.proxy.percentage` | `int` | `0`
| `hhvm.proxy.proxy_patterns` | `Vector<string>` | *empty*
| `hhvm.proxy.proxy_urls` | `Set<string>` | *empty*
| `hhvm.proxy.retry` | `int` | 3
| `hhvm.proxy.serve_urls` | `Set<string>` | *empty*

### Other

Here are some other HHVM settings that may be useful in an advanced situation.

| Setting | Type | Default | Description
|---------|------|---------|------------
| `hhvm.allow_hhas` | `bool` | `false` | Allow executing HHVM Assembly (hhas) files, which is code written directly in [HHVM Bytecode (HHBC)](https://github.com/facebook/hhvm/blob/master/hphp/doc/bytecode.specification). Directly writing HHBC can easily crash HHVM, and is rarely a useful thing to do anyways.
| `hhvm.spin_on_crash` | `bool` | `false` | If enabled, when HHVM crashes, this will wait for the debugger to attach to the pid.
| `hhvm.simulate_arm` | `bool` | `false` | If enabled, this will allow HHVM to simulate running on ARM architecture.
| `hhvm.timeouts_use_wall_time` | `bool` | `true` | Determines whether or not to interpret set_time_limit timeouts as wall time or CPU time.
| `hhvm.dynamic_extension_path` | `string` | `.` | path to look for extensions if a fully qualified path is not provided. The current path is the default.
| `hhvm.gdb_sync_chunks` | `int` | `128` | This forces the VM to sync debug info synchronously with gdb, this many chunks at a time.
| `hhvm.hhbc_arena_chunk_size` | `int` | `10485760` (1 MB) | The chunk size for the HHBC arena.
| `hhvm.initial_named_entity_table_size` | `int` | `30000` | The initial size of the named entity table.
| `hhvm.initial_static_string_table_size` | `int` | `500000` | The initial size of the static string table.
| `hhvm.map_tc_huge` | `bool` | `false`
| `hhvm.map_tgt_cache_huge` | `bool` | `false`
| `hhvm.max_low_mem_huge_pages` | `int` | `0`
| `hhvm.vm_initial_global_table_size` | `int` | `512` | The initial global table size.s
| `hhvm.vm_stack_elms` | `int` | `16384` | The maximum stack size.

## Unused Settings

These are settings that are currently not used in the codebase.

| Setting | Type | Default |
|---------|------|---------|
| `hhvm.enable_alternative` | `int` | `0`
| `hhvm.server.enable_cuf_async` | `bool` | `false`
| `hhvm.server.lib_event_sync_send` | `bool` | `true`
| `hhvm.server.response_queue_count` | `int` | `0`
| `hhvm.server.shutdown_listen_no_work` | `int` | `-1`
| `hhvm.debug.full_backtrace` | `bool` | `false`
| `hhvm.debug.local_memcache` | `bool` | `false`
| `hhvm.debug.memcache_read_only` | `bool` | `false`
| `hhvm.debug.translate_leak_stack_trace` | `bool` | `false`
| `hhvm.debug.translate_source` | `bool` | `false` | Used to translate C++ file and line numbers into original PHP file and line numbers.
| `hhvm.error_handling.enable_hip_hop_errors` | `bool` | `true`
| `hhvm.error_handling.max_loop_count` | `int` | `0`
| `hhvm.error_handling.warn_too_many_arguments` | `bool` | `false`
| `hhvm.hack_array_warn_frequency` | `int` | `0`
| `hhvm.resource_limit.max_memcache_key_count` | `int` | `0`
| `hhvm.resource_limit.string_offset_limit` | `int` | `10 * 1024 * 1024` (10 MB)
