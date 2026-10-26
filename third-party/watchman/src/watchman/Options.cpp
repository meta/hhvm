/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Options.h"
#include <fmt/core.h>
#include <string.h>
#include "watchman/CommandRegistry.h"
#include "watchman/LogConfig.h"
#include "watchman/Logging.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/portability/GetOpt.h"

#define IS_REQUIRED(x) (x) == REQ_STRING

namespace watchman {

Flags flags;

namespace {
const OptDesc opts[] = {
    {"help",
     'h',
     "Show this help",
     OPT_NONE,
     &flags.show_help,
     nullptr,
     NOT_DAEMON},
#ifndef _WIN32
    {"inetd",
     0,
     "Spawning from an inetd style supervisor",
     OPT_NONE,
     &flags.inetd_style,
     nullptr,
     IS_DAEMON},
#endif
    {"no-site-spawner",
     'S',
     "Don't use the site or system spawner",
     OPT_NONE,
     &flags.no_site_spawner,
     nullptr,
     IS_DAEMON},
    {"version",
     'v',
     "Show version number",
     OPT_NONE,
     &flags.show_version,
     NULL,
     NOT_DAEMON},
/* -U / --sockname  have legacy meaning; unix domain on unix,
 * named pipe path on windows.  After we chose this assignment,
 * Windows evolved unix domain support which muddies this.
 * We need to preserve the sockname/U option here for backwards
 * compatibility */
#ifdef _WIN32
    {"sockname",
     'U',
     "DEPRECATED: Specify alternate named pipe path (specifying this will"
     " disable unix domain sockets unless `--unix-listener-path` is"
     " specified)",
     REQ_STRING,
     &flags.named_pipe_path,
     "PATH",
     IS_DAEMON},
#else
    {"sockname",
     'U',
     "DEPRECATED: Specify alternate sockname. Use `--unix-listener-path` instead.",
     REQ_STRING,
     &flags.unix_sock_name,
     "PATH",
     IS_DAEMON},
#endif
    {"named-pipe-path",
     0,
     "Specify alternate named pipe path",
     REQ_STRING,
     &flags.named_pipe_path,
     "PATH",
     IS_DAEMON},
    {"unix-listener-path",
     'u',
#ifdef _WIN32
     "Specify alternate unix domain socket path (specifying this will disable"
     " named pipes unless `--named-pipe-path` is specified)",
#else
     "Specify alternate unix domain socket path",
#endif
     REQ_STRING,
     &flags.unix_sock_name,
     "PATH",
     IS_DAEMON},
    {"logfile",
     'o',
     "Specify path to logfile ('-' = stdout and stderr)",
     REQ_STRING,
     &watchman::logging::log_name,
     "PATH",
     IS_DAEMON},
    {"log-level",
     0,
     "set the log level (0 = off, default is 1, verbose = 2)",
     REQ_INT,
     &watchman::logging::log_level,
     NULL,
     IS_DAEMON},
    {"pidfile",
     0,
     "Specify path to pidfile",
     REQ_STRING,
     &flags.pid_file,
     "PATH",
     IS_DAEMON},
    {"persistent",
     'p',
     "Persist and wait for further responses",
     OPT_NONE,
     &flags.persistent,
     NULL,
     NOT_DAEMON},
    {"no-save-state",
     'n',
     "Don't save state between invocations",
     OPT_NONE,
     &flags.dont_save_state,
     NULL,
     IS_DAEMON},
    {"statefile",
     0,
     "Specify path to file to hold watch and trigger state",
     REQ_STRING,
     &flags.watchman_state_file,
     "PATH",
     IS_DAEMON},
    {"json-command",
     'j',
     "Instead of parsing CLI arguments, take a single "
     "json object from stdin",
     OPT_NONE,
     &flags.json_input_arg,
     NULL,
     NOT_DAEMON},
    {"output-encoding",
     0,
     "CLI output encoding. json (default) or bser",
     REQ_STRING,
     &flags.output_encoding,
     NULL,
     NOT_DAEMON},
    {"server-encoding",
     0,
     "CLI<->server encoding. bser (default) or json",
     REQ_STRING,
     &flags.server_encoding,
     NULL,
     NOT_DAEMON},
    {"foreground",
     'f',
     "Run the service in the foreground",
     OPT_NONE,
     &flags.foreground,
     NULL,
     NOT_DAEMON},
    {"pretty",
     0,
     "Force pretty output, even if stdout isn't a TTY",
     OPT_NONE,
     &flags.yes_pretty,
     NULL,
     NOT_DAEMON},
    {"no-pretty",
     0,
     "Don't pretty print JSON",
     OPT_NONE,
     &flags.no_pretty,
     NULL,
     NOT_DAEMON},
    {"no-spawn",
     0,
     "Don't try to start the service if it is not available",
     OPT_NONE,
     &flags.no_spawn,
     NULL,
     NOT_DAEMON},
    {"no-local",
     0,
     "When no-spawn is enabled, don't try to handle request"
     " in client mode if service is unavailable",
     OPT_NONE,
     &flags.no_local,
     NULL,
     NOT_DAEMON},
    // test-state-dir is for testing only and should not be used in production:
    // instead, use the compile-time WATCHMAN_STATE_DIR option
    {"test-state-dir",
     0,
     NULL,
     REQ_STRING,
     &flags.test_state_dir,
     "DIR",
     NOT_DAEMON},
    {0, 0, 0, OPT_NONE, 0, 0, 0},
};
} // namespace

void print_command_list_for_help(FILE* where) {
  auto defs = CommandDefinition::getAll();
  std::sort(
      defs.begin(),
      defs.end(),
      [](const CommandDefinition* A, const CommandDefinition* B) {
        return A->name < B->name;
      });

  fprintf(where, "\n\nAvailable commands:\n\n");
  for (auto& def : defs) {
    fmt::print(where, "      {}\n", def->name);
  }
}

/* One does not simply use getopt_long() */

[[noreturn]] void usage(const OptDesc* opts_2, FILE* where) {
  int i;
  size_t len;
  size_t longest = 0;
  const char* label;

  fprintf(where, "Usage: watchman [opts_2] command\n");

  /* measure up option names so we can format nicely */
  for (i = 0; opts_2[i].optname; i++) {
    label = opts_2[i].arglabel ? opts_2[i].arglabel : "ARG";

    len = strlen(opts_2[i].optname);
    switch (opts_2[i].argtype) {
      case REQ_STRING:
        len += strlen(label) + strlen("=");
        break;
      default:;
    }

    if (opts_2[i].shortopt) {
      len += strlen("-X, ");
    }

    if (len > longest) {
      longest = len;
    }
  }

  /* space between option definition and help text */
  longest += 3;

  for (i = 0; opts_2[i].optname; i++) {
    char buf[80];

    if (!opts_2[i].helptext) {
      // This is a signal that this option shouldn't be printed out.
      continue;
    }

    label = opts_2[i].arglabel ? opts_2[i].arglabel : "ARG";

    fprintf(where, "\n ");
    if (opts_2[i].shortopt) {
      fprintf(where, "-%c, ", opts_2[i].shortopt);
    } else {
      fprintf(where, "    ");
    }
    switch (opts_2[i].argtype) {
      case REQ_STRING:
        snprintf(buf, sizeof(buf), "--%s=%s", opts_2[i].optname, label);
        break;
      default:
        snprintf(buf, sizeof(buf), "--%s", opts_2[i].optname);
        break;
    }

    fprintf(where, "%-*s ", (unsigned int)longest, buf);

    fprintf(where, "%s", opts_2[i].helptext);
    fprintf(where, "\n");
  }

  print_command_list_for_help(where);

  fprintf(
      where,
      "\n"
      "See https://github.com/facebook/watchman#watchman for more help\n"
      "\n"
      "Watchman, by Wez Furlong.\n"
      "Copyright (c) Meta Platforms, Inc.\n");

  exit(1);
}

std::vector<std::string>
w_getopt(const OptDesc* opts_2, int* argcp, char*** argvp) {
  int num_opts, i;
  char* nextshort;
  int argc = *argcp;
  char** argv = *argvp;
  int long_pos = -1;
  int res;

  /* first build up the getopt_long bits that we need */
  for (num_opts = 0; opts_2[num_opts].optname; num_opts++) {
    ;
  }

  /* to hold the args we pass to the daemon */
  std::vector<std::string> daemon_argv;

  /* something to hold the long options */
  auto long_opts = (option*)calloc(num_opts + 1, sizeof(struct option));
  if (!long_opts) {
    log(FATAL, "calloc struct option\n");
  }

  /* and the short options */
  auto shortopts = (char*)malloc((1 + num_opts) * 2);
  if (!shortopts) {
    log(FATAL, "malloc shortopts\n");
  }
  nextshort = shortopts;
  nextshort[0] = ':';
  nextshort++;

  /* now transfer information into the space we made */
  for (i = 0; i < num_opts; i++) {
    long_opts[i].name = (char*)opts_2[i].optname;
    long_opts[i].val = opts_2[i].shortopt;
    switch (opts_2[i].argtype) {
      case OPT_NONE:
        long_opts[i].has_arg = no_argument;
        break;
      case REQ_STRING:
      case REQ_INT:
        long_opts[i].has_arg = required_argument;
        break;
    }

    if (opts_2[i].shortopt) {
      nextshort[0] = (char)opts_2[i].shortopt;
      nextshort++;

      if (long_opts[i].has_arg != no_argument) {
        nextshort[0] = ':';
        nextshort++;
      }
    }
  }

  nextshort[0] = 0;

  while ((res = getopt_long(argc, argv, shortopts, long_opts, &long_pos)) !=
         -1) {
    const OptDesc* o;

    switch (res) {
      case ':':
        /* missing option argument.
         * Check to see if it was actually optional */
        for (long_pos = 0; long_pos < num_opts; long_pos++) {
          if (opts_2[long_pos].shortopt == optopt) {
            if (IS_REQUIRED(opts_2[long_pos].argtype)) {
              fprintf(
                  stderr,
                  "--%s (-%c) requires an argument",
                  opts_2[long_pos].optname,
                  opts_2[long_pos].shortopt);
              return daemon_argv;
            }
          }
        }
        break;

      case '?':
        /* unknown option */
        fprintf(stderr, "Unknown or invalid option! %s\n", argv[optind - 1]);
        usage(opts_2, stderr);

      default:
        if (res == 0) {
          /* we got a long option */
          o = &opts_2[long_pos];
        } else {
          /* map short option to the real thing */
          o = NULL;
          for (long_pos = 0; long_pos < num_opts; long_pos++) {
            if (opts_2[long_pos].shortopt == res) {
              o = &opts_2[long_pos];
              break;
            }
          }
        }

        if (o->is_daemon) {
          daemon_argv.push_back(
              fmt::format("--{}={}", o->optname, optarg ? optarg : ""));
        }

        /* store the argument if we found one */
        if (o->argtype != OPT_NONE && o->val && optarg) {
          switch (o->argtype) {
            case REQ_INT: {
              auto ival = atoi(optarg);
              *(int*)o->val = ival;
              break;
            }
            case REQ_STRING: {
              auto sval = typed_string_to_json(optarg, W_STRING_UNICODE);
              *(std::string*)o->val = optarg;
              break;
            }
            case OPT_NONE:;
          }
        }
        if (o->argtype == OPT_NONE && o->val) {
          auto bval = json_true();
          *(int*)o->val = 1;
        }
    }

    long_pos = -1;
  }

  free(long_opts);
  free(shortopts);

  *argcp = argc - optind;
  *argvp = argv + optind;
  return daemon_argv;
}

std::vector<std::string> parseOptions(int* argcp, char*** argvp) {
  auto daemon_argv = w_getopt(opts, argcp, argvp);
  if (flags.show_help) {
    usage(opts, stdout);
  }
  if (flags.show_version) {
    fmt::print("{}\n", PACKAGE_VERSION);
    exit(0);
  }
  return daemon_argv;
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
