/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::fs::DirEntry;
use std::fs::File;
use std::io;
use std::io::BufRead;
use std::io::BufReader;
use std::io::Write;
use std::path::Path;
use std::path::PathBuf;

use anyhow::Result;
use clap::Parser;
use duct::Expression;
use sysinfo::System;
#[cfg(target_os = "linux")]
use tabular::Table;
#[cfg(target_os = "linux")]
use tabular::row;
use watchman_client::CanonicalPath;
use watchman_client::Client;
use watchman_client::Connector;
use watchman_client::ResolvedRoot;
use watchman_client::prelude::GetConfigRequest;

#[cfg(feature = "fb")]
mod facebook;
mod stream;

use self::stream::Stream;
use crate::audit::AuditOption;

// Wrapper type around [`duct::Expression`] to provide better error messages
struct RageExpression {
    expr: Expression,
    cmd: &'static str,
}

impl RageExpression {
    fn new(expr: Expression, cmd: &'static str) -> Self {
        Self {
            expr: expr.stderr_to_stdout(),
            cmd,
        }
    }

    fn config<T: FnOnce(Expression) -> Expression>(mut self, fun: T) -> Self {
        self.expr = fun(self.expr);
        self
    }

    fn read(&self) -> String {
        self.expr.read().map_or_else(
            |e| format!("Failed to run '{}': {:?}", self.cmd, e),
            |x| x.trim().to_string(),
        )
    }
}

macro_rules! cmd {
    ($program:expr $(, $arg:expr )* $(,)? ) => {
        RageExpression::new(duct::cmd!($program, $($arg),*), stringify!($program $($arg)*))
    };
}

#[cfg(unix)]
fn getuid() -> nix::unistd::Uid {
    nix::unistd::getuid()
}

struct WatchmanRage {
    stream: Stream,
}

impl WatchmanRage {
    async fn new() -> Self {
        let hostname = System::host_name().ok();
        let stream = Stream::new(hostname);

        Self { stream }
    }

    fn empty_line(&mut self) -> Result<()> {
        writeln!(self.stream)?;
        Ok(())
    }

    async fn run(&mut self) -> Result<()> {
        self.print_system_info()?;
        self.empty_line()?;
        self.print_package_version()?;
        self.empty_line()?;
        self.print_cli_version()?;
        self.empty_line()?;
        self.print_watchman_env()?;
        self.empty_line()?;
        #[cfg(target_os = "linux")]
        {
            self.print_inotify()?;
            self.empty_line()?;
        }
        #[cfg(target_os = "macos")]
        {
            self.print_launchd_info()?;
            self.empty_line()?;
        }
        self.print_state_info()?;
        self.empty_line()?;
        #[cfg(any(target_os = "linux", target_os = "macos"))]
        {
            self.print_running_watchman()?;
            self.empty_line()?;
        }
        self.print_watchman_service_info().await?;
        self.empty_line()?;
        Ok(())
    }

    fn print_system_info(&mut self) -> Result<()> {
        macro_rules! write_or_unknown {
            ($out: expr, $fmt: expr, $value: expr) => {
                writeln!($out, $fmt, $value.as_deref().unwrap_or("<unknown>"))
            };
        }

        // Note: OS & Arch here are set at compile time, which can be inaccurate in some cases (Apple Silicon)
        writeln!(self.stream, "Platform: {}", std::env::consts::OS)?;
        writeln!(
            self.stream,
            "Arch (Compile Time): {}",
            std::env::consts::ARCH
        )?;
        write_or_unknown!(self.stream, "Hostname: {}", System::host_name())?;
        write_or_unknown!(self.stream, "Release: {}", System::name())?;
        write_or_unknown!(self.stream, "System Version: {}", System::os_version())?;
        write_or_unknown!(self.stream, "Kernel Version: {}", System::kernel_version())?;
        #[cfg(unix)]
        writeln!(self.stream, "Running watchman-diag as UID: {}", getuid())?;

        Ok(())
    }

    #[cfg(unix)]
    fn print_package_version(&mut self) -> Result<()> {
        writeln!(
            self.stream,
            "RPM version (rpm -q fb-watchman): {}",
            cmd!("rpm", "-q", "fb-watchman").read()
        )?;
        Ok(())
    }

    #[cfg(windows)]
    fn print_package_version(&mut self) -> Result<()> {
        writeln!(
            self.stream,
            "Chocolatey version (clist -lr fb.watchman): {}",
            cmd!("clist", "-lr", "fb.watchman").read()
        )?;
        Ok(())
    }

    fn print_cli_version(&mut self) -> Result<()> {
        writeln!(
            self.stream,
            "CLI version (watchman -v): {}",
            cmd!("watchman", "--no-spawn", "-v").read()
        )?;
        Ok(())
    }

    fn print_watchman_env(&mut self) -> Result<()> {
        let vars = std::env::vars()
            .filter(|(k, _)| k.starts_with("WATCHMAN_"))
            .collect::<Vec<_>>();

        if !vars.is_empty() {
            writeln!(
                self.stream,
                "WARNING: The following Watchman related environment variables are set (this is unusual and may cause problems):"
            )?;

            for (k, v) in vars.iter() {
                writeln!(self.stream, "{}={}", k, v)?;
            }
        }

        Ok(())
    }

    #[cfg(target_os = "linux")]
    fn print_inotify(&mut self) -> Result<()> {
        let mut table =
            Table::new("{:<} {:<} {:<} {:<}").with_row(row!("PID", "EXE", "FD", "WATCHES"));

        macro_rules! bail {
            ($e: expr) => {
                match $e {
                    Ok(v) => v,
                    Err(_) => continue,
                }
            };
        }
        let procs = match std::fs::read_dir("/proc") {
            Ok(c) => c,
            Err(e) => {
                writeln!(self.stream, "Unable to crawl inotify information: {:?}", e)?;
                return Ok(());
            }
        };
        for proc in procs {
            let proc = bail!(proc).path();
            let content = bail!(std::fs::read_dir(proc.join("fd")));
            let exe = std::fs::read_link(proc.join("exe")).map(|x| x.display().to_string());
            let exe = exe.as_deref().unwrap_or("<unknown>");

            for entry in content {
                let fd = bail!(entry).path();
                let target = bail!(std::fs::read_link(&fd));
                if target.display().to_string() != "anon_inode:inotify" {
                    continue;
                }

                if let Some(fdnum) = fd.file_name() {
                    let fdinfo = proc.join("fdinfo").join(fdnum);
                    let fdinfo = bail!(std::fs::read_to_string(fdinfo));
                    let fdnum = fdnum.to_string_lossy();
                    let pid = proc
                        .file_name()
                        .map_or_else(|| "<unknown>".into(), |x| x.to_string_lossy());

                    if fdinfo.is_empty() {
                        table.add_row(row!(pid, exe, fdnum, "<unknown>"));
                    } else {
                        let watches = fdinfo
                            .lines()
                            .filter(|line| line.starts_with("inotify wd:"))
                            .count();
                        table.add_row(row!(pid, exe, fdnum, watches));
                    }
                }
            }
        }

        writeln!(self.stream, "Inotify watch information")?;
        writeln!(self.stream, "{}", table)?;
        Ok(())
    }

    #[cfg(target_os = "macos")]
    fn print_launchd_info(&mut self) -> Result<()> {
        writeln!(self.stream, "Launchd info:")?;
        writeln!(
            self.stream,
            "{}",
            cmd!("launchctl", "list", "com.github.facebook.watchman").read()
        )?;

        writeln!(self.stream, "launchd.log samples:")?;
        match File::open("/var/log/com.apple.xpc.launchd/launchd.log") {
            Ok(file) => {
                let watchman_lines = BufReader::new(file)
                    .lines()
                    .filter_map(|result| {
                        if let Ok(line) = result {
                            if line.contains("com.github.facebook.watchman") {
                                return Some(line);
                            }
                        }
                        None
                    })
                    .take(40); // Limit log entries printed to avoid flooding the rage

                for line in watchman_lines {
                    writeln!(self.stream, "{}", line)?;
                }
            }
            Err(e) => {
                writeln!(self.stream, "Failed to open launchd.log: {}", e)?;
            }
        }

        Ok(())
    }

    fn print_state_info(&mut self) -> Result<()> {
        // construct all possible state directories
        let mut roots: Vec<PathBuf> = vec![
            "/var/facebook/watchman".into(),
            "/opt/facebook/var/run/watchman".into(),
            "/opt/facebook/watchman/var/run/watchman".into(),
        ];

        if let Ok(path) = std::env::var("TEMP") {
            roots.push(path.into());
        }
        if let Ok(path) = std::env::var("TMP") {
            roots.push(path.into());
        }

        /// Are there any file ends with `-state` name in there
        fn get_state_dirs(p: &Path) -> Vec<DirEntry> {
            if let Ok(content) = std::fs::read_dir(p) {
                content
                    .filter_map(|entry| {
                        if let Ok(entry) = entry {
                            if entry.file_name().to_string_lossy().ends_with("-state") {
                                Some(entry)
                            } else {
                                None
                            }
                        } else {
                            None
                        }
                    })
                    .collect::<Vec<_>>()
            } else {
                Vec::new()
            }
        }

        fn print_state_dir(out: &mut Stream, state: &Path) -> Result<()> {
            let state_file = state.join("state");
            writeln!(out, "State information from {}\n", state.display())?;
            writeln!(out, "State file: {}", state_file.display())?;

            if let Ok(content) = std::fs::read_to_string(state_file) {
                writeln!(out, "{}", content)?;
            }

            let log_file = state.join("log");
            if let Ok(lines) = {
                File::open(&log_file).and_then(|file| {
                    // We prefer `.split` here instead of `.lines` since the log
                    // file may contain invalid utf-8 characters.
                    BufReader::new(file)
                        .split(b'\n')
                        .collect::<io::Result<Vec<_>>>()
                })
            } {
                writeln!(out, "Log samples: {}", log_file.display())?;

                for line in lines.into_iter().rev().take(300).rev() {
                    writeln!(out, "{}", String::from_utf8_lossy(&line).trim())?;
                }
            }

            Ok(())
        }

        for root in roots.iter() {
            let dirs = get_state_dirs(root);
            for state in dirs {
                print_state_dir(&mut self.stream, &state.path())?;
            }
        }

        #[cfg(windows)]
        if let Ok(path) = std::env::var("LOCALAPPDATA") {
            let windows_state = PathBuf::from(&path).join("watchman");
            print_state_dir(&mut self.stream, &windows_state)?;
        }

        Ok(())
    }

    #[cfg(any(target_os = "linux", target_os = "macos"))]
    fn print_running_watchman(&mut self) -> Result<()> {
        let lines = duct::cmd!("ps", "-ef").read()?;

        writeln!(self.stream, "Running Watchman Processes")?;
        for line in lines.lines() {
            if line.contains("watchman") {
                writeln!(self.stream, "{}", line)?;
            }
        }

        Ok(())
    }

    async fn print_watchman_service_info(&mut self) -> Result<()> {
        // Do not run if we are in sudo
        #[cfg(posix)]
        if getuid == 0 && std::env::var("SUDO_UID").is_some() {
            return;
        }

        // TODO: connect timeself.stream
        let client = Connector::new().connect().await?;
        let version = client.version().await?;

        writeln!(self.stream, "Watchman service information:")?;
        writeln!(self.stream, "{:?}", version)?;
        writeln!(self.stream, "Status:\n")?;

        writeln!(
            self.stream,
            "{}",
            cmd!("watchman", "--pretty", "debug-status").read()
        )?;

        // TODO(zeyi): it's probably better if this can return `ResolvedRoot`
        let watches = client.watch_list().await?.roots;
        writeln!(self.stream, "Watches:")?;
        for watch in watches.iter() {
            writeln!(self.stream, "- {}", watch.display())?;
        }

        for watch in watches.iter() {
            writeln!(self.stream)?;

            let option = AuditOption {
                silent: true,
                ..Default::default()
            };
            if let Err(e) = crate::audit::audit_repo(&mut self.stream, &client, watch, option).await
            {
                writeln!(
                    self.stream,
                    "Failed to sanity check {}: {:?}",
                    watch.display(),
                    e
                )?;
            }
            writeln!(self.stream)?;

            self.collect_watch_info(&client, watch).await.ok();
        }

        Ok(())
    }

    async fn check_watchman_config(&mut self, client: &Client, root: &ResolvedRoot) -> Result<()> {
        let repo_root = root.project_root();
        // We are not using `client.get_config()` method to examine the
        // configuration currently used in Watchman server because it is typed
        // and we may need to compare fields that is not yet included in the struct definition.
        let repo_config: serde_json::Value = client
            .generic_request(GetConfigRequest("get-config", repo_root.into()))
            .await?;
        let watchmanconfig_file = repo_root.join(".watchmanconfig");
        let watchmanconfig = std::fs::read_to_string(&watchmanconfig_file)?;
        let watchmanconfig: serde_json::Value = serde_json::from_str(&watchmanconfig)?;

        if let Some(repo_config) = repo_config.get("config") {
            if repo_config != &watchmanconfig {
                writeln!(
                    self.stream,
                    "Watchman root {} is using this configuration:\n {}",
                    repo_root.display(),
                    repo_config
                )?;
                writeln!(
                    self.stream,
                    "'{}' has this configuration:\n{}",
                    watchmanconfig_file.display(),
                    watchmanconfig
                )?;
                writeln!(
                    self.stream,
                    "** You should run: `watchman watch-del {}; watchman watch {}` to reload .watchmanconfig **",
                    repo_root.display(),
                    repo_root.display()
                )?;
            }
        } else {
            writeln!(
                self.stream,
                "Failed to retireve configuration from Watchman. Not checking if configuration matches on-disk setting"
            )?;
        }

        Ok(())
    }

    async fn collect_watch_info(&mut self, client: &Client, repo: &Path) -> Result<()> {
        let root = match client
            .resolve_root(CanonicalPath::with_canonicalized_path(repo.into()))
            .await
        {
            Ok(root) => root,
            Err(e) => {
                writeln!(
                    self.stream,
                    "Failed to resolve repo root for '{}': {:?}\n",
                    repo.display(),
                    e
                )?;
                return Ok(());
            }
        };

        if let Err(e) = self.check_watchman_config(client, &root).await {
            writeln!(
                self.stream,
                "Failed to check Watchman configuration for '{}': {:?}\n",
                repo.display(),
                e
            )?;
        }

        if root.watcher() != "eden" {
            writeln!(self.stream, "Sparse configuration for {}", repo.display())?;
            writeln!(
                self.stream,
                "{}\n",
                cmd!("hg", "sparse").config(|c| c.dir(repo)).read()
            )?;

            // TODO(zeyi): we could migrate this into using the watchman connection,
            // but we need to define the struct first
            writeln!(
                self.stream,
                "Content hash cache stats for {}",
                repo.display()
            )?;
            writeln!(
                self.stream,
                "{}\n",
                cmd!("watchman", "debug-contenthash", repo).read()
            )?;

            writeln!(
                self.stream,
                "Symlink target cache stats for {}",
                repo.display()
            )?;
            writeln!(
                self.stream,
                "{}\n",
                cmd!("watchman", "debug-symlink-target-cache", repo).read()
            )?;
        }

        writeln!(self.stream, "Subscriptions for {}", repo.display())?;
        writeln!(
            self.stream,
            "{}\n",
            cmd!("watchman", "debug-get-subscriptions", repo).read()
        )?;

        writeln!(self.stream, "Asserted states for {}", repo.display())?;
        writeln!(
            self.stream,
            "{}\n",
            cmd!("watchman", "debug-get-asserted-states", repo).read()
        )?;

        Ok(())
    }

    fn wait(self) {
        self.stream.wait();
    }
}

#[derive(Parser, Debug)]
pub(crate) struct RageCmd {}

impl RageCmd {
    pub(crate) async fn run(&self) -> Result<()> {
        let mut rage = WatchmanRage::new().await;
        rage.run().await?;
        rage.wait();

        Ok(())
    }
}
