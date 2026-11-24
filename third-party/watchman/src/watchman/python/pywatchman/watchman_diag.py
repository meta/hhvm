# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


# Collect some FB specific watchman diagnostics
import glob
import json
import os
import stat
import subprocess
import sys
import time

import pywatchman


def print_table(table):
    col_width = [max(len(x) for x in col) for col in zip(*table)]
    for line in table:
        print(
            "  ".join("{0:{1}}".format(x, col_width[i]) for i, x in enumerate(line))
            + " "
        )


def print_status(msg):
    print(msg)
    # If they're piping into `arc paste`, they may get impatient
    if not os.isatty(sys.stdout.fileno()):
        sys.stderr.write(msg + "\n")


class ProcessInfo:
    def __init__(self):
        self.pid_to_name = {}

    def procname(self, pid):
        if pid in self.pid_to_name:
            return self.pid_to_name[pid]
        name = os.readlink("/proc/%s/exe" % pid)
        try:
            name = os.path.realpath(name)
        except Exception:
            pass
        self.pid_to_name[pid] = name
        return name


class InotifyInfo:
    """Extract information about users of inotify on the system"""

    def __init__(self, procinfo):
        self.procinfo = procinfo

    def procname(self, pid):
        return self.procinfo.procname(pid)

    def read_fdinfo(self, path):
        bufsize = 65536
        blob = []
        fd = os.open(path, os.O_RDONLY)
        while True:
            buf = os.read(fd, bufsize)
            if len(buf) == 0:
                break
            blob.append(buf)
        os.close(fd)
        if len(blob) == 0:
            return None
        return "".join(blob)

    def parsefdinfo(self, blob):
        watches = 0
        for line in blob.split("\n"):
            if line.find("inotify wd") != -1:
                watches = watches + 1
        return watches

    def get_watches(self):
        watches = [("PID", "EXE", "FD", "WATCHES")]
        for fddir in glob.glob("/proc/*/fd"):
            for fdnode in glob.glob(fddir + "/*"):
                try:
                    link = os.readlink(fdnode)
                    if link != "anon_inode:inotify":
                        continue
                    _, _, pid, _, fdnum = fdnode.split("/")
                    info = self.read_fdinfo("/proc/%s/fdinfo/%s" % (pid, fdnum))
                    if info is None:
                        watches.append(
                            (pid, self.procname(pid), fdnum, "<unknown> (see t8692428)")
                        )
                        continue
                    watches.append(
                        (pid, self.procname(pid), fdnum, str(self.parsefdinfo(info)))
                    )

                except Exception:
                    pass
        return watches


def walk_root(root, case_sensitive, ignores):
    """Generate a map of file nodes for the given dir by looking
    at the filesystem"""

    # we can't use os.walk because it insists on stating and derefing
    # dewey and gvfs symlinks (== slow)
    results = {}

    # the queue of dirs to analyze
    dirs = [root]
    while len(dirs) > 0:
        dir = dirs.pop()
        for ent in os.listdir(dir):
            full = os.path.join(dir, ent)
            rel = os.path.relpath(full, root)

            if rel in ignores:
                continue

            st = os.lstat(full)
            if stat.S_ISDIR(st.st_mode):
                # add this child to our dir queue
                dirs.append(full)

            item = (rel, st)
            if not case_sensitive:
                rel = rel.lower()
            results[rel] = item

    return results


def collect_watch_info(watchman, watch):
    root_config = watchman.query("get-config", watch)["config"]
    watchmanconfig_file = os.path.join(watch, ".watchmanconfig")
    file_config = {}
    if os.path.exists(watchmanconfig_file):
        with open(watchmanconfig_file) as f:
            file_config = json.load(f)
    if file_config != root_config:
        print("Watchman root %s is using this configuration: %s" % (watch, root_config))
        print("%s has this configuration: %s" % (watchmanconfig_file, file_config))
        print_status(
            (
                "** You should run: `watchman watch-del %s ; "
                + "watchman watch %s` to reload .watchmanconfig **\n"
            )
            % (watch, watch)
        )

    if not is_eden(watch):
        # Eden mounts don't use the sparse extension, so skip this bit
        print("\nSparse configuration for %s:" % watch)
        passthru("cd %s && hg sparse" % watch, shell=True)

        # Eden watcher is stateless and doesn't have this
        print("\nContent hash cache stats for %s:" % watch)
        passthru(["watchman", "debug-contenthash", watch])

        print("\nSymlink target cache stats for %s:" % watch)
        passthru(["watchman", "debug-symlink-target-cache", watch])

    print("\nSubscriptions for %s:" % watch)
    passthru(["watchman", "debug-get-subscriptions", watch])

    print("\nAsserted states for %s:" % watch)
    passthru(["watchman", "debug-get-asserted-states", watch])


def is_eden(dirpath):
    if sys.platform == "win32":
        return os.path.isfile(os.path.join(dirpath, ".eden", "config"))
    return os.path.islink(os.path.join(dirpath, ".eden", "root"))


def cross_check_watch(watchman, watch, case_sensitive):
    if is_eden(watch):
        # We don't keep any state in watchman for eden mounts
        # that is worth testing against an O(repo) crawl
        print_status(
            "\nSkipping filesystem sanity check for %s as it is an eden mount\n" % watch
        )
        return

    print_status(
        "\nSanity checking the filesystem at %s against watchman; this may take a couple of minutes."
        % watch
    )

    root_config = watchman.query("get-config", watch)["config"]
    ignores = []
    if "ignore_dirs" in root_config:
        ignores = root_config["ignore_dirs"]
    ignores.append(".hg")
    ignores.append(".git")
    ignores.append(".svn")

    print_status("Crawling %s..." % watch)
    start = time.time()
    fs = walk_root(watch, case_sensitive, ignores)
    print_status("(took %ds)" % (time.time() - start))
    start = time.time()
    print_status("Interrogating watchman about %s..." % watch)

    fields = ["name", "mode", "size", "mtime_f", "oclock"]
    if os.name == "posix":
        fields.append("ino")

    files = watchman.query(
        "query",
        watch,
        {
            "expression": [
                "allof",
                [
                    "not",
                    [
                        "anyof",
                        ["dirname", ".git"],
                        ["dirname", ".hg"],
                        ["dirname", ".svn"],
                        ["name", ".git", "wholename"],
                        ["name", ".svn", "wholename"],
                        ["name", ".hg", "wholename"],
                    ],
                ],
                "exists",
            ],
            "fields": fields,
        },
    )
    print_status("(took %ds)" % (time.time() - start))
    print_status("Comparing results...")

    phantoms = []
    missing = []
    all_names_in_watchman = set()

    def diff_item(w_item, fs_item):
        diffs = []
        if w_item["name"] != fs_item[0]:
            diffs.append(
                "watchman name is `%s` vs fs `%s" % (w_item["name"], fs_item[0])
            )
        st = fs_item[1]
        if w_item["mode"] != st.st_mode:
            diffs.append(
                "watchman mode is 0%o vs fs 0%o" % (w_item["mode"], st.st_mode)
            )
        if w_item["size"] != st.st_size and not stat.S_ISDIR(st.st_mode):
            diffs.append("watchman size is %d vs fs %d" % (w_item["size"], st.st_size))
        if w_item["mtime_f"] != st.st_mtime:
            diffs.append(
                "watchman mtime is %s vs fs %s" % (w_item["mtime_f"], st.st_mtime)
            )
        if os.name == "posix" and (w_item["ino"] != st.st_ino):
            diffs.append("watchman ino is %d vs fs %d" % (w_item["ino"], st.st_ino))

        if len(diffs) > 0:
            diffs.append("   oclock is %s" % w_item["oclock"])
            return diffs
        return None

    for f in files["files"]:
        key = f["name"]
        if not case_sensitive:
            key = key.lower()
        all_names_in_watchman.add(key)

        if key not in fs:
            phantoms.append(f)
        else:
            diff = diff_item(f, fs[key])
            if diff:
                print("Conflicting information for %s:" % f["name"])
                for d in diff:
                    print(d)

    for key in fs:
        if key not in all_names_in_watchman:
            missing.append(fs[key])

    print_status(
        "There are %d items reported by watchman that do not exist on the fs:"
        % len(phantoms)
    )
    if len(phantoms) > 0:
        for item in phantoms:
            print(item)

    print_status(
        "There are %d items on the filesystem not reported by watchman:" % len(missing)
    )
    if len(missing) > 0:
        # Let's see if watchman had previously seen any of these
        names = ["anyof"]
        for item in missing:
            name = item[0]
            print(name, item[1])
            names.append(["name", name, "wholename"])

        files = watchman.query("query", watch, {"expression": names, "fields": fields})

        print("This is what watchman knows about this set of files:")
        for f in files["files"]:
            print(f)


def passthru(*args, **kwargs):
    sys.stdout.flush()
    try:
        subprocess.call(*args, **kwargs)
    except Exception as e:
        print("Error while running %s %s: %s" % (args, kwargs, e))


def collect_state_info(path):
    try:
        print("State information from %s" % path)
        print
        print("State file: %s/state" % path)
        try:
            with open(os.path.join(path, "state"), "r") as f:
                print(f.read())
        except FileNotFoundError:
            pass

        try:
            with open(os.path.join(path, "log"), "r") as f:
                lines = f.readlines()
                tail = lines[-300:]
                print("Log samples: %s/log" % path)
                for line in tail:
                    print(line.rstrip())
        except FileNotFoundError:
            pass
    except Exception as e:
        print("# %s" % str(e))
        pass
    print


def main() -> None:
    if not os.isatty(sys.stdout.fileno()):
        sys.stderr.write(
            "(most output is going to your pipe, some short summary will show here on stderr)\n"
        )

    # Print the basic system info
    print("Platform: %s" % sys.platform)
    if os.name == "posix":
        uname = os.uname()
        print_table([uname])
        print("Running watchman-diag as uid %d" % os.getuid())

    print

    if os.name == "posix":
        print("RPM version: (rpm -q fb-watchman)")
        passthru(["rpm", "-q", "fb-watchman"])

    if sys.platform == "win32":
        print("choco package version:")
        passthru(["choco", "list", "--local-only", "watchman"])

    print("CLI version: (watchman -v)")
    passthru(["watchman", "--no-spawn", "-v"])
    print

    watchman_env_vars = [v for v in os.environ.keys() if v.startswith("WATCHMAN_")]
    if watchman_env_vars:
        print()
        print(
            "!!!WARNING!!! The following watchman related environment variables are set",
            "(this is unusual and may cause problems):",
        )
        for var in watchman_env_vars:
            print("%s=%s" % (var, os.getenv(var)))
        print()

    procinfo = ProcessInfo()
    case_sensitive = True

    if sys.platform == "linux":
        inotify = InotifyInfo(procinfo)
        print("Inotify watch information:")
        print_table(inotify.get_watches())
        print

    if sys.platform == "darwin":
        print("launchd info:")
        passthru(["launchctl", "list", "com.github.facebook.watchman"])
        case_sensitive = False

    if sys.platform == "win32":
        case_sensitive = False

    for root in [
        "/var/facebook/watchman",
        "/opt/facebook/var/run/watchman",
        "/opt/facebook/watchman/var/run/watchman",
        os.environ.get("TEMP"),
        os.environ.get("TMP"),
    ]:
        if root is None or root == "":
            continue

        for path in glob.glob("%s/*-state" % root):
            collect_state_info(path)

    appdata = os.environ.get("LOCALAPPDATA")
    if appdata:
        watchman_appdata = os.path.join(appdata, "watchman")
        if os.path.exists(watchman_appdata):
            collect_state_info(watchman_appdata)

    if os.name == "posix":
        print("List of running watchman processes:")
        passthru("ps -ef | grep watchman", shell=True)
        print

    # We do these last, as they depend on watchman being able to respond.
    # Skip when running as root under sudo, to avoid touching root-owned state.
    # os.getuid is POSIX-only; keep it inside the os.name guard so the type checker
    # doesn't flag it against the Windows typeshed.
    safe_to_run = True
    if os.name == "posix":
        if os.getuid() == 0 and os.environ.get("SUDO_UID"):
            safe_to_run = False
    if safe_to_run:
        # Safe to run watchman commands
        print("Watchman service information:")
        watchman = pywatchman.client(timeout=600)
        print(watchman.query("version"))
        print("\nStatus:\n")
        passthru(["watchman", "--pretty", "debug-status"])

        watches = watchman.query("watch-list")["roots"]
        print("Watches:\n%s\n" % watches)

        for watch in watches:
            cross_check_watch(watchman, watch, case_sensitive)
            collect_watch_info(watchman, watch)


if __name__ == "__main__":
    main()
