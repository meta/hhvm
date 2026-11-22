# Using ini

ini usage in HHVM is fairly similar to that of php-src, albeit currently with
some limitations and enhancements. For example, HHVM currently doesn't support
per-dir ini settings (support coming soon), but it does support vector-based
settings and wildcards for copying and symlinking values from other settings.

## Common usage

Here is the typical format of an ini file (values not necessarily realistic):

```
hhvm.hot_func_count = 11
hhvm.stats.slot_duration = 11
hhvm.env_variables["MYBOOL"] = true
hhvm.env_variables["MYINT"] = 5
hhvm.env_variables["MYSTR"] = "Custom String"
hhvm.env_variables["ANOTHERSTR"] = "Another String"
hhvm.server_variables[] = "Key will be next available int"
hhvm.server_variables[] = "Value will be next available string"
hhvm.error_handling.notice_frequency = 1
hhvm.error_handling.warning_frequency = 1
hhvm.enable_obj_destruct_call = true
hhvm.enable_xhp = true
```

## Copying and Symlinking Settings

**NOTE**: This feature currently only works with core system settings. They
don't yet work with extensions, `ini_set()`, etc.

You can also provide wildcards to settings signaling that you want to use the
value of another setting for its value.

* `@`: Copy the value directly into this setting.
* `:`: Symlink the value from the other setting to this setting. If the other
setting changes, then this setting will change with it, and vice-versa.

To use this feature, use the form

```
hhvm.setting[any-sub-key | @ | :][...] = value | "hhvm.substitution-setting"
```

e.g.,

```
hhvm.a = 3
hhvm.b[@] = "hhvm.a"
hhvm.c[d][@] = "hhvm.a"
```

Here is a more complete example:

```
hhvm.hot_func_count = 11
hhvm.stats.slot_duration[@] = "hhvm.hot_func_count"
hhvm.server.allowed_exec_cmds[0] = "ls"
hhvm.server.allowed_exec_cmds[1][@]= "hhvm.env_variables[MYSTR]"
hhvm.server.allowed_exec_cmds[2][@]= "hhvm.env_variables[ANOTHERSTR]"
hhvm.env_variables["MYBOOL"] = true
hhvm.env_variables["MYINT"][:] = "hhvm.hot_func_count"
hhvm.env_variables["MYSTR"] = "Custom String"
hhvm.env_variables["ANOTHERSTR"] = "Another String"
hhvm.server_variables[0] = "Key will be next available int"
hhvm.server_variables[1][@] = "hhvm.server.allowed_exec_cmds[0]"
hhvm.error_handling.notice_frequency = 1
hhvm.error_handling.warning_frequency[:] = "hhvm.error_handling.notice_frequency"
hhvm.enable_obj_destruct_call = true
hhvm.enable_xhp[@]= "hhvm.enable_obj_destruct_call"
```

**NOTE**: If you using this feature with vector or map based settings where you
can specify `[]` to indicate an in-order increment of a setting, you must
specify explicit indices for them because they will be used to determine which values to be used when copying or symlinking.
