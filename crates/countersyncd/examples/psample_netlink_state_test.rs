//! Privileged local Generic Netlink state-machine stress test using psample.
//!
//! Run with:
//!
//! `sudo env "HOME=$HOME" "PATH=$PATH:/usr/sbin:/sbin" CARGO_TARGET_DIR=/tmp/countersyncd-psample-target "$(command -v cargo)" run -p countersyncd --example psample_netlink_state_test --release`
//!
//! This is intentionally an example instead of a Cargo test: normal CI compiles it via
//! `cargo check --all-targets`, but does not execute host-wide module unload/reload operations.

use std::{
    collections::BTreeMap,
    error::Error,
    fs,
    future::Future,
    io,
    os::fd::{AsRawFd, FromRawFd, OwnedFd},
    path::PathBuf,
    process::{Command, Output, Stdio},
    sync::OnceLock,
    time::{Duration, Instant as WallInstant},
};

use countersyncd::{
    actor::{control_netlink::ControlNetlinkActor, data_netlink::DataNetlinkActor},
    message::{buffer::SocketBufferMessage, netlink::NetlinkCommand},
};
use log::LevelFilter;
use tokio::{
    io::unix::AsyncFd,
    runtime::Builder,
    sync::mpsc::{channel, Receiver, Sender},
    task::JoinHandle,
    time::advance,
};

const FAMILY: &str = "psample";
const GROUP: &str = "packets";
const SAMPLE_GROUP: u32 = 0x5a17;
const DEFAULT_RELOADS: usize = 128;
const MIN_RELOADS: usize = 101;
const MAX_RSS_GROWTH_KIB: u64 = 32 * 1024;
const MAX_HEAP_GROWTH_BYTES: usize = 2 * 1024 * 1024;
const MAX_HEAP_TREND_BYTES: u64 = 512 * 1024;
const MAX_RSS_TREND_KIB: u64 = 8 * 1024;
const COMMAND_TIMEOUT: Duration = Duration::from_secs(10);
const PSAMPLE_ATTR_SAMPLE_GROUP: u16 = 3;
const PSAMPLE_ATTR_DATA: u16 = 6;
const NLA_TYPE_MASK: u16 = 0x3fff;

const OUTAGES: &[(&str, Duration)] = &[
    ("100ms", Duration::from_millis(100)),
    ("1s", Duration::from_secs(1)),
    ("10s", Duration::from_secs(10)),
    ("30s", Duration::from_secs(30)),
    ("1h", Duration::from_secs(60 * 60)),
    ("1d", Duration::from_secs(24 * 60 * 60)),
    ("1w", Duration::from_secs(7 * 24 * 60 * 60)),
    ("1mo", Duration::from_secs(30 * 24 * 60 * 60)),
    ("1y", Duration::from_secs(365 * 24 * 60 * 60)),
];

type DynError = Box<dyn Error + Send + Sync>;

struct Actors {
    command_sender: Sender<NetlinkCommand>,
    data_receiver: Receiver<SocketBufferMessage>,
    data_task: JoinHandle<()>,
    control_task: JoinHandle<()>,
}

struct Cleanup {
    active: bool,
    introduced_helpers: Vec<&'static str>,
}

struct StressStats {
    reloads: usize,
    max_fds: usize,
    final_rss: u64,
    max_rss: u64,
    max_heap: usize,
}

struct WallTimer(AsyncFd<OwnedFd>);

struct AutoAdvanceGuard {
    release: Option<std::sync::mpsc::Sender<()>>,
    task: Option<JoinHandle<()>>,
}

impl WallTimer {
    fn new(duration: Duration) -> Result<Self, DynError> {
        let fd = unsafe {
            libc::timerfd_create(
                libc::CLOCK_MONOTONIC,
                libc::TFD_CLOEXEC | libc::TFD_NONBLOCK,
            )
        };
        if fd < 0 {
            return Err(io::Error::last_os_error().into());
        }

        let fd = unsafe { OwnedFd::from_raw_fd(fd) };
        let timer = libc::itimerspec {
            it_interval: libc::timespec {
                tv_sec: 0,
                tv_nsec: 0,
            },
            it_value: libc::timespec {
                tv_sec: duration.as_secs().try_into()?,
                tv_nsec: duration.subsec_nanos().try_into()?,
            },
        };
        if unsafe { libc::timerfd_settime(fd.as_raw_fd(), 0, &timer, std::ptr::null_mut()) } < 0 {
            return Err(io::Error::last_os_error().into());
        }
        Ok(Self(AsyncFd::new(fd)?))
    }

    async fn elapsed(&self) -> Result<(), DynError> {
        loop {
            let mut ready = self.0.readable().await?;
            let mut expirations = 0u64;
            let read = unsafe {
                libc::read(
                    self.0.get_ref().as_raw_fd(),
                    (&mut expirations as *mut u64).cast(),
                    std::mem::size_of::<u64>(),
                )
            };
            if read == std::mem::size_of::<u64>() as isize {
                return Ok(());
            }
            let error = io::Error::last_os_error();
            if error.kind() == io::ErrorKind::WouldBlock {
                ready.clear_ready();
                continue;
            }
            return Err(error.into());
        }
    }
}

impl AutoAdvanceGuard {
    async fn new() -> Self {
        let (release, receiver) = std::sync::mpsc::channel();
        let task = tokio::task::spawn_blocking(move || {
            let _ = receiver.recv();
        });
        tokio::task::yield_now().await;
        Self {
            release: Some(release),
            task: Some(task),
        }
    }

    async fn stop(mut self) -> Result<(), DynError> {
        drop(self.release.take());
        self.task.take().expect("auto-advance guard task").await?;
        Ok(())
    }
}

impl Drop for AutoAdvanceGuard {
    fn drop(&mut self) {
        drop(self.release.take());
    }
}

impl Cleanup {
    fn new() -> Result<Self, DynError> {
        if module_loaded("psample")? || module_loaded("act_sample")? {
            return Err(
                "psample and act_sample must be initially unloaded; use an exclusive test host"
                    .into(),
            );
        }
        ensure_test_links_absent()?;
        let introduced_helpers = ["veth", "sch_ingress", "cls_matchall"]
            .into_iter()
            .filter(|module| !module_loaded(module).unwrap_or(false))
            .collect();
        Ok(Self {
            active: true,
            introduced_helpers,
        })
    }

    fn finish(&mut self) -> Result<(), DynError> {
        delete_links()?;
        unload_modules()?;
        unload_helper_modules(&self.introduced_helpers)?;
        self.active = false;
        Ok(())
    }
}

impl Drop for Cleanup {
    fn drop(&mut self) {
        if self.active {
            let _ = delete_links();
            let _ = unload_modules();
            let _ = unload_helper_modules(&self.introduced_helpers);
        }
    }
}

fn main() -> Result<(), DynError> {
    let runtime = Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|e| format!("create Tokio runtime: {e}"))?;
    runtime.block_on(run_test())
}

async fn run_test() -> Result<(), DynError> {
    let _ = env_logger::builder()
        .filter_module("countersyncd::actor::control_netlink", LevelFilter::Error)
        .filter_module("countersyncd::actor::data_netlink", LevelFilter::Error)
        .try_init();
    require_root()?;
    require_command("modprobe")?;
    require_command("modinfo")?;
    require_command("rmmod")?;
    require_command("ip")?;
    require_command("tc")?;
    require_module("psample")?;
    require_module("act_sample")?;
    let mut sigint = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::interrupt())?;
    let mut sigterm = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;

    let mut cleanup = Cleanup::new()?;
    let reloads = reloads()?;

    eprintln!("loading psample and configuring the sampling path");
    load_modules()?;
    require_removable_modules()?;
    create_sample_path()?;

    let pre_actor_fds = fd_snapshot()?;
    let mut actors = start_actors();
    if let Err(error) = wait_for_data_socket(&mut actors).await {
        let shutdown = stop_actors(actors, false).await;
        let cleanup_result = cleanup.finish();
        return Err(format!(
            "initial psample receive failed: {error}; actor shutdown: {shutdown:?}; host cleanup: {cleanup_result:?}"
        )
        .into());
    }

    tokio::time::pause();
    let auto_advance_guard = AutoAdvanceGuard::new().await;
    verify_auto_advance_disabled().await?;
    let stress_result = {
        let stress = run_stress(&mut actors, reloads);
        tokio::pin!(stress);
        tokio::select! {
            result = &mut stress => result,
            _ = sigint.recv() => Err("interrupted by SIGINT".into()),
            _ = sigterm.recv() => Err("interrupted by SIGTERM".into()),
        }
    };

    let shutdown_result = stop_actors(actors, true).await;
    let guard_result = auto_advance_guard.stop().await;
    let post_actor_fds = fd_snapshot();
    let cleanup_result = cleanup.finish();

    let mut failures = Vec::new();
    let stats = match stress_result {
        Ok(stats) => Some(stats),
        Err(error) => {
            failures.push(format!("stress test failed: {error}"));
            None
        }
    };
    if let Err(error) = shutdown_result {
        failures.push(format!("actor shutdown failed: {error}"));
    }
    match post_actor_fds {
        Ok(post_actor_fds) if post_actor_fds != pre_actor_fds => failures.push(format!(
            "actor shutdown changed fd set: before={pre_actor_fds:?}, after={post_actor_fds:?}"
        )),
        Err(error) => failures.push(format!("post-shutdown fd snapshot failed: {error}")),
        _ => {}
    }
    if let Err(error) = guard_result {
        failures.push(format!("virtual-time guard shutdown failed: {error}"));
    }
    if let Err(error) = cleanup_result {
        failures.push(format!("host cleanup failed: {error}"));
    }
    if !failures.is_empty() {
        return Err(failures.join("; ").into());
    }
    let stats = stats.expect("stress stats are present when no failures were recorded");

    println!(
        "passed {} reloads; max_fds={}, final_rss_kib={}, max_rss_kib={}, max_heap_bytes={}",
        stats.reloads, stats.max_fds, stats.final_rss, stats.max_rss, stats.max_heap
    );
    Ok(())
}

async fn run_stress(actors: &mut Actors, reloads: usize) -> Result<StressStats, DynError> {
    verify_sample(actors, b"initial-psample").await?;

    println!("warming all outage durations before leak baselining");
    for &(name, outage) in OUTAGES {
        cycle_family(actors, usize::MAX, name, outage).await?;
    }

    trim_allocator();
    let baseline_fds = fd_snapshot()?;
    let baseline_fd_count: usize = baseline_fds.values().sum();
    let baseline_rss = rss_kib()?;
    let baseline_heap = heap_in_use_bytes();
    let mut max_fds = baseline_fd_count;
    let mut max_rss = baseline_rss;
    let mut max_heap = baseline_heap;
    let mut rss_samples = Vec::with_capacity(reloads);
    let mut heap_samples = Vec::with_capacity(reloads);

    println!(
        "psample state test: reloads={reloads}, baseline_fds={max_fds}, baseline_rss_kib={baseline_rss}, baseline_heap_bytes={baseline_heap}"
    );

    for iteration in 0..reloads {
        let (name, outage) = OUTAGES[iteration % OUTAGES.len()];
        cycle_family(actors, iteration, name, outage).await?;

        trim_allocator();
        let current_fds = fd_snapshot()?;
        let current_fd_count = current_fds.values().sum();
        let current_rss = rss_kib()?;
        let current_heap = heap_in_use_bytes();
        max_fds = max_fds.max(current_fd_count);
        max_rss = max_rss.max(current_rss);
        max_heap = max_heap.max(current_heap);
        rss_samples.push(current_rss);
        heap_samples.push(current_heap as u64);

        if current_fds != baseline_fds {
            return Err(
                format!("fd leak: baseline={baseline_fds:?}, current={current_fds:?}").into(),
            );
        }
        if max_rss > baseline_rss + MAX_RSS_GROWTH_KIB {
            return Err(format!(
                "memory growth exceeded limit: baseline={baseline_rss} KiB, max={max_rss} KiB"
            )
            .into());
        }
        if max_heap > baseline_heap + MAX_HEAP_GROWTH_BYTES {
            return Err(format!(
                "heap growth exceeded limit: baseline={baseline_heap} bytes, max={max_heap} bytes"
            )
            .into());
        }

        println!(
            "iteration {}/{} passed; fds={current_fd_count}, rss_kib={current_rss}, heap_bytes={current_heap}",
            iteration + 1,
            reloads
        );
    }

    check_growth_trend("RSS", &rss_samples, MAX_RSS_TREND_KIB)?;
    check_growth_trend("heap", &heap_samples, MAX_HEAP_TREND_BYTES)?;
    Ok(StressStats {
        reloads: reloads + OUTAGES.len(),
        max_fds,
        final_rss: *rss_samples.last().unwrap_or(&baseline_rss),
        max_rss,
        max_heap,
    })
}

fn check_growth_trend(name: &str, samples: &[u64], limit: u64) -> Result<(), DynError> {
    let window = (samples.len() / 4).max(1);
    let first = median(&samples[..window]);
    let last = median(&samples[samples.len() - window..]);
    if last > first + limit {
        return Err(format!(
            "{name} shows sustained growth: first-window median={first}, last-window median={last}, limit={limit}"
        )
        .into());
    }
    Ok(())
}

fn median(samples: &[u64]) -> u64 {
    let mut samples = samples.to_vec();
    samples.sort_unstable();
    samples[samples.len() / 2]
}

async fn cycle_family(
    actors: &mut Actors,
    iteration: usize,
    outage_name: &str,
    outage: Duration,
) -> Result<(), DynError> {
    eprintln!("cycle {iteration}: unloading psample for {outage_name}");
    let old_ids = resolve_ids()?;
    delete_links()?;
    unload_modules()?;
    expect_family(false)?;

    let notification_time = outage.min(Duration::from_millis(100));
    advance_in_steps(notification_time).await;
    advance(outage - notification_time).await;
    settle().await;
    if family_exists()? {
        return Err(format!("psample unexpectedly exists during {outage_name} outage").into());
    }

    load_modules()?;
    expect_family(true)?;
    let new_ids = resolve_ids()?;
    create_sample_path()?;
    advance_in_steps(Duration::from_millis(100)).await;

    let marker = format!("psample-{iteration}-{outage_name}");
    verify_sample(actors, marker.as_bytes()).await?;
    println!(
        "  {outage_name}: family/group IDs {:?} -> {:?}",
        old_ids, new_ids
    );
    Ok(())
}

fn start_actors() -> Actors {
    let (command_sender, command_receiver) = channel(32);
    let (data_sender, data_receiver) = channel(32);

    let mut data_actor = DataNetlinkActor::new(FAMILY, GROUP, command_receiver, 4 * 1024 * 1024);
    data_actor.add_recipient(data_sender);
    let control_actor = ControlNetlinkActor::new(FAMILY, command_sender.clone());

    Actors {
        command_sender,
        data_receiver,
        data_task: tokio::spawn(DataNetlinkActor::run(data_actor)),
        control_task: tokio::spawn(ControlNetlinkActor::run(control_actor)),
    }
}

async fn stop_actors(actors: Actors, time_paused: bool) -> Result<(), DynError> {
    let close_result = wall_timeout(
        Duration::from_secs(5),
        actors.command_sender.send(NetlinkCommand::Close),
    )
    .await;
    drop(actors.command_sender);

    let data_result = stop_task("data actor", actors.data_task).await;
    if time_paused {
        advance_in_steps(Duration::from_millis(20)).await;
    }
    let control_result = stop_task("control actor", actors.control_task).await;

    close_result??;
    data_result?;
    control_result?;
    Ok(())
}

async fn stop_task(name: &str, mut task: JoinHandle<()>) -> Result<(), DynError> {
    match wall_timeout(Duration::from_secs(5), &mut task).await {
        Ok(result) => result.map_err(|error| format!("{name} failed: {error}").into()),
        Err(error) => {
            task.abort();
            let _ = task.await;
            Err(format!("{name} did not stop: {error}").into())
        }
    }
}

async fn wait_for_data_socket(actors: &mut Actors) -> Result<(), DynError> {
    for attempt in 0..20 {
        let marker = format!("startup-{attempt}");
        send_marker(marker.as_bytes())?;
        if let Ok(message) =
            wall_timeout(Duration::from_millis(100), actors.data_receiver.recv()).await
        {
            let message = message.ok_or("data actor channel closed during startup")?;
            if sample_contains(&message, marker.as_bytes())? {
                return Ok(());
            }
        }
        wall_timeout(
            Duration::from_secs(1),
            actors.command_sender.send(NetlinkCommand::Reconnect),
        )
        .await?
        .map_err(|e| format!("request startup reconnect: {e}"))?;
        settle().await;
    }
    Err("data actor did not receive psample data during startup".into())
}

async fn verify_sample(actors: &mut Actors, marker: &[u8]) -> Result<(), DynError> {
    for _ in 0..20 {
        send_marker(marker)?;
        let result = wall_timeout(Duration::from_millis(250), actors.data_receiver.recv()).await;
        if let Ok(Some(message)) = result {
            if sample_contains(&message, marker)? {
                return Ok(());
            }
        }
        settle().await;
    }

    Err(format!(
        "data actor did not receive marker {:?} after psample reload",
        String::from_utf8_lossy(marker)
    )
    .into())
}

fn sample_contains(message: &SocketBufferMessage, marker: &[u8]) -> Result<bool, DynError> {
    let mut offset = 0usize;
    let mut sample_group = None;
    let mut data = None;

    while offset + 4 <= message.len() {
        let len = u16::from_ne_bytes([message[offset], message[offset + 1]]) as usize;
        let kind = u16::from_ne_bytes([message[offset + 2], message[offset + 3]]) & NLA_TYPE_MASK;
        if len < 4 || offset + len > message.len() {
            return Err(format!("invalid psample attribute at offset {offset}: len={len}").into());
        }
        let value = &message[offset + 4..offset + len];
        match kind {
            PSAMPLE_ATTR_SAMPLE_GROUP if value.len() >= 4 => {
                sample_group = Some(u32::from_ne_bytes(value[..4].try_into()?));
            }
            PSAMPLE_ATTR_DATA => data = Some(value),
            _ => {}
        }
        offset += (len + 3) & !3;
    }

    Ok(sample_group == Some(SAMPLE_GROUP)
        && data.is_some_and(|packet| packet.windows(marker.len()).any(|window| window == marker)))
}

fn create_sample_path() -> Result<(), DynError> {
    delete_links()?;
    let tx = tx_link();
    let rx = rx_link();
    let sample_group = SAMPLE_GROUP.to_string();
    run(
        "ip",
        &["link", "add", tx, "type", "veth", "peer", "name", rx],
    )?;
    run("ip", &["link", "set", tx, "up"])?;
    run("ip", &["link", "set", rx, "up"])?;
    run("tc", &["qdisc", "add", "dev", rx, "clsact"])?;
    run(
        "tc",
        &[
            "filter",
            "add",
            "dev",
            rx,
            "ingress",
            "matchall",
            "action",
            "sample",
            "rate",
            "1",
            "group",
            &sample_group,
            "trunc",
            "256",
        ],
    )?;
    Ok(())
}

fn send_marker(marker: &[u8]) -> Result<(), DynError> {
    let socket = unsafe {
        libc::socket(
            libc::AF_PACKET,
            libc::SOCK_RAW | libc::SOCK_CLOEXEC,
            (libc::ETH_P_ALL as u16).to_be().into(),
        )
    };
    if socket < 0 {
        return Err(io::Error::last_os_error().into());
    }

    let result = (|| {
        let tx = std::ffi::CString::new(tx_link())?;
        let ifindex = unsafe { libc::if_nametoindex(tx.as_ptr()) };
        if ifindex == 0 {
            return Err(io::Error::last_os_error());
        }

        let mut frame = vec![0u8; 14 + marker.len()];
        frame[..6].fill(0xff);
        frame[6..12].copy_from_slice(&[0x02, 0, 0, 0, 0, 1]);
        frame[12..14].copy_from_slice(&0x88b5u16.to_be_bytes());
        frame[14..].copy_from_slice(marker);

        let address = libc::sockaddr_ll {
            sll_family: libc::AF_PACKET as u16,
            sll_protocol: 0x88b5u16.to_be(),
            sll_ifindex: ifindex as i32,
            sll_hatype: 0,
            sll_pkttype: 0,
            sll_halen: 6,
            sll_addr: [0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0],
        };

        let sent = unsafe {
            libc::sendto(
                socket,
                frame.as_ptr().cast(),
                frame.len(),
                0,
                (&address as *const libc::sockaddr_ll).cast(),
                std::mem::size_of::<libc::sockaddr_ll>() as libc::socklen_t,
            )
        };
        if sent != frame.len() as isize {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    })();

    unsafe { libc::close(socket) };
    result.map_err(Into::into)
}

fn load_modules() -> Result<(), DynError> {
    run("modprobe", &["psample"])?;
    run("modprobe", &["act_sample"])?;
    Ok(())
}

fn unload_modules() -> Result<(), DynError> {
    let deadline = WallInstant::now() + Duration::from_secs(5);
    loop {
        let _ = run("rmmod", &["act_sample"]);
        let _ = run("rmmod", &["psample"]);
        if !module_loaded("act_sample")? && !module_loaded("psample")? {
            return Ok(());
        }
        if WallInstant::now() >= deadline {
            return Err(
                "could not unload act_sample/psample; another user may hold a reference".into(),
            );
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn unload_helper_modules(modules: &[&str]) -> Result<(), DynError> {
    for module in modules.iter().rev() {
        if module_loaded(module)? {
            run("rmmod", &[*module])?;
        }
    }
    Ok(())
}

fn require_removable_modules() -> Result<(), DynError> {
    let psample = output("modinfo", &["-F", "filename", "psample"])?;
    let act_sample = output("modinfo", &["-F", "filename", "act_sample"])?;
    if !psample.status.success() || !act_sample.status.success() {
        return Err("modinfo could not locate psample/act_sample module files".into());
    }
    let psample = String::from_utf8_lossy(&psample.stdout);
    let act_sample = String::from_utf8_lossy(&act_sample.stdout);
    if psample.trim() == "(builtin)" || act_sample.trim() == "(builtin)" {
        return Err("psample and act_sample must be modules, not built into the kernel".into());
    }

    Ok(())
}

fn delete_links() -> Result<(), DynError> {
    for name in [tx_link(), rx_link()] {
        if link_exists(name)? {
            let result = output("ip", &["link", "del", name])?;
            if !result.status.success() && link_exists(name)? {
                return Err(command_error("ip", &["link", "del", name], &result).into());
            }
        }
    }
    Ok(())
}

fn ensure_test_links_absent() -> Result<(), DynError> {
    for name in [tx_link(), rx_link()] {
        if link_exists(name)? {
            return Err(format!("refusing to delete pre-existing interface {name}").into());
        }
    }
    Ok(())
}

fn tx_link() -> &'static str {
    static NAME: OnceLock<String> = OnceLock::new();
    NAME.get_or_init(|| format!("pst{:x}", std::process::id()))
}

fn rx_link() -> &'static str {
    static NAME: OnceLock<String> = OnceLock::new();
    NAME.get_or_init(|| format!("psr{:x}", std::process::id()))
}

fn expect_family(expected: bool) -> Result<(), DynError> {
    let actual = family_exists()?;
    if actual != expected {
        return Err(format!(
            "psample family is {}, expected {}",
            if actual { "available" } else { "absent" },
            if expected { "available" } else { "absent" }
        )
        .into());
    }
    Ok(())
}

fn family_exists() -> Result<bool, DynError> {
    let mut socket = countersyncd::actor::netlink_utils::create_nl_resolver()
        .ok_or("create Generic Netlink resolver")?;
    match countersyncd::actor::netlink_utils::resolve_family_id(&mut socket, FAMILY) {
        Ok(_) => Ok(true),
        Err(error) if error.raw_os_error() == Some(libc::ENOENT) => Ok(false),
        Err(error) => Err(format!("resolve psample family: {error}").into()),
    }
}

fn resolve_ids() -> Result<(u16, u32), DynError> {
    let mut family_socket = countersyncd::actor::netlink_utils::create_nl_resolver()
        .ok_or("create Generic Netlink family resolver")?;
    let family_id =
        countersyncd::actor::netlink_utils::resolve_family_id(&mut family_socket, FAMILY)?;

    let mut group_socket = countersyncd::actor::netlink_utils::create_nl_resolver()
        .ok_or("create Generic Netlink group resolver")?;
    let group_id = countersyncd::actor::netlink_utils::resolve_multicast_group(
        &mut group_socket,
        FAMILY,
        GROUP,
    )?;
    Ok((family_id, group_id))
}

fn module_loaded(module: &str) -> Result<bool, DynError> {
    Ok(fs::read_to_string("/proc/modules")?
        .lines()
        .any(|line| line.split_whitespace().next() == Some(module)))
}

fn link_exists(name: &str) -> Result<bool, DynError> {
    Ok(output("ip", &["link", "show", "dev", name])?
        .status
        .success())
}

fn require_root() -> Result<(), DynError> {
    if unsafe { libc::geteuid() } != 0 {
        return Err("run this example as root (sudo -E cargo run ...)".into());
    }
    Ok(())
}

fn require_command(command: &str) -> Result<(), DynError> {
    if command_path(command).is_none() {
        return Err(format!("required command not found: {command}").into());
    }
    Ok(())
}

fn require_module(module: &str) -> Result<(), DynError> {
    let result = output("modprobe", &["-n", module])?;
    if !result.status.success() {
        return Err(command_error("modprobe", &["-n", module], &result).into());
    }
    Ok(())
}

fn reloads() -> Result<usize, DynError> {
    let reloads = match std::env::var("PSAMPLE_TEST_RELOADS") {
        Ok(value) => value.parse()?,
        Err(_) => DEFAULT_RELOADS,
    };
    if reloads == 0 {
        return Err("PSAMPLE_TEST_RELOADS must be greater than zero".into());
    }
    if reloads < MIN_RELOADS && std::env::var_os("PSAMPLE_TEST_SMOKE").is_none() {
        return Err(format!("PSAMPLE_TEST_RELOADS must be at least {MIN_RELOADS}").into());
    }
    Ok(reloads)
}

fn fd_snapshot() -> Result<BTreeMap<String, usize>, DynError> {
    let mut snapshot = BTreeMap::new();
    for entry in fs::read_dir("/proc/self/fd")? {
        let entry = entry?;
        let target = match fs::read_link(entry.path()) {
            Ok(target) => normalize_fd_target(&target.display().to_string()),
            Err(error) if error.kind() == io::ErrorKind::NotFound => continue,
            Err(error) => return Err(error.into()),
        };
        *snapshot.entry(target).or_default() += 1;
    }
    Ok(snapshot)
}

fn normalize_fd_target(target: &str) -> String {
    if target.starts_with("socket:[") {
        "socket".to_string()
    } else if target.starts_with("anon_inode:[eventpoll]") {
        "eventpoll".to_string()
    } else if target.starts_with("anon_inode:[timerfd]") {
        "timerfd".to_string()
    } else {
        target.to_string()
    }
}

fn rss_kib() -> Result<u64, DynError> {
    let status = fs::read_to_string("/proc/self/status")?;
    let value = status
        .lines()
        .find_map(|line| line.strip_prefix("VmRSS:"))
        .and_then(|line| line.split_whitespace().next())
        .ok_or("VmRSS missing from /proc/self/status")?;
    Ok(value.parse()?)
}

async fn settle() {
    for _ in 0..4 {
        tokio::task::yield_now().await;
    }
}

async fn verify_auto_advance_disabled() -> Result<(), DynError> {
    let before = tokio::time::Instant::now();
    let virtual_timer = tokio::time::sleep(Duration::from_secs(1));
    tokio::pin!(virtual_timer);
    let wall_timer = WallTimer::new(Duration::from_millis(20))?;
    tokio::select! {
        _ = &mut virtual_timer => {
            return Err("Tokio time auto-advanced despite the guard".into());
        }
        result = wall_timer.elapsed() => result?,
    }
    if tokio::time::Instant::now() != before {
        return Err("Tokio time advanced during a real wall-clock wait".into());
    }
    Ok(())
}

async fn advance_in_steps(duration: Duration) {
    let mut remaining = duration;
    while !remaining.is_zero() {
        let step = remaining.min(Duration::from_millis(10));
        advance(step).await;
        settle().await;
        remaining -= step;
    }
}

async fn wall_timeout<F: Future>(duration: Duration, future: F) -> Result<F::Output, DynError> {
    let timer = WallTimer::new(duration)?;
    tokio::pin!(future);

    tokio::select! {
        result = &mut future => Ok(result),
        result = timer.elapsed() => {
            result?;
            Err(format!("wall-clock timeout after {duration:?}").into())
        },
    }
}

fn trim_allocator() {
    unsafe {
        libc::malloc_trim(0);
    }
}

fn heap_in_use_bytes() -> usize {
    unsafe { libc::mallinfo2().uordblks }
}

fn run(command: &str, args: &[&str]) -> Result<(), DynError> {
    let result = output(command, args)?;
    if result.status.success() {
        Ok(())
    } else {
        Err(command_error(command, args, &result).into())
    }
}

fn output(command: &str, args: &[&str]) -> Result<Output, DynError> {
    let executable =
        command_path(command).ok_or_else(|| format!("required command not found: {command}"))?;
    let mut child = Command::new(executable)
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("run {command} {}: {e}", args.join(" ")))?;
    let deadline = WallInstant::now() + COMMAND_TIMEOUT;
    loop {
        if child.try_wait()?.is_some() {
            return child
                .wait_with_output()
                .map_err(|e| format!("collect {command} output: {e}").into());
        }
        if WallInstant::now() >= deadline {
            child.kill()?;
            let _ = child.wait();
            return Err(format!(
                "{} {} timed out after {:?}",
                command,
                args.join(" "),
                COMMAND_TIMEOUT
            )
            .into());
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn command_path(command: &str) -> Option<PathBuf> {
    let mut directories = std::env::var_os("PATH")
        .map(|path| std::env::split_paths(&path).collect::<Vec<_>>())
        .unwrap_or_default();
    directories.extend([PathBuf::from("/usr/sbin"), PathBuf::from("/sbin")]);
    directories
        .into_iter()
        .map(|directory| directory.join(command))
        .find(|path| path.is_file())
}

fn command_error(command: &str, args: &[&str], output: &Output) -> String {
    format!(
        "{} {} failed with {}: {}",
        command,
        args.join(" "),
        output.status,
        String::from_utf8_lossy(&output.stderr).trim()
    )
}
