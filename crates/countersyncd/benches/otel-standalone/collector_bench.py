#!/usr/bin/env python3
"""Linux experiment runner; explicit paths, bounded subprocesses, ACK/count checks.

Collector must have otlp receiver, nop exporter, pprof extension (core v0.123.0).
Example: python3 collector_bench.py --collector /path/to/otelcol --client
/path/to/otel-throughput --output /tmp/results --inflight 1 2 4 8 16
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.request


def metrics():
    text = urllib.request.urlopen("http://127.0.0.1:28888/metrics", timeout=5).read().decode()
    result = {}
    for key in ("otelcol_receiver_accepted_metric_points", "otelcol_receiver_refused_metric_points",
                "otelcol_process_cpu_seconds", "otelcol_process_runtime_total_alloc_bytes",
                "otelcol_process_memory_rss_bytes"):
        result[key] = sum(float(line.split()[-1]) for line in text.splitlines()
                          if not line.startswith("#") and line.split("{")[0].split()[0]
                          in (key, key + "_total"))
    return result


def profile(output, seconds):
    for _ in range(600):
        if metrics()["otelcol_receiver_accepted_metric_points"] > 0:
            break
        time.sleep(.1)
    else:
        raise RuntimeError("no incoming metrics for profile")
    with urllib.request.urlopen(f"http://127.0.0.1:21777/debug/pprof/profile?seconds={seconds}",
                                timeout=seconds + 15) as response:
        output.with_suffix(".cpu.pprof").write_bytes(response.read())
    for kind in ("allocs", "heap"):
        with urllib.request.urlopen(f"http://127.0.0.1:21777/debug/pprof/{kind}", timeout=15) as response:
            output.with_suffix(f".{kind}.pprof").write_bytes(response.read())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--collector", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("collector.yaml"))
    parser.add_argument("--inflight", type=int, nargs="+", default=[1, 2, 4, 8, 16])
    parser.add_argument("--batch", type=int, default=10000)
    parser.add_argument("--points", type=int, default=40000000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--cpus", default="2-15")
    parser.add_argument("--gomaxprocs", type=int, default=12)
    parser.add_argument("--gogc", default="100")
    parser.add_argument("--client-cpus", default="0", help="comma-separated CPUs for multithread binary")
    parser.add_argument("--profile-seconds", type=int, default=0)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for inflight in args.inflight:
        assert inflight > 0
        name = f"b{args.batch}-n{inflight}-p{args.gomaxprocs}-gc{args.gogc}-{time.time_ns()}"
        output = args.output / name
        with output.with_suffix(".log").open("w") as log:
            collector = subprocess.Popen(
                ["taskset", "-c", args.cpus, str(args.collector.resolve()), "--config", str(args.config.resolve())],
                env=dict(os.environ, GOMAXPROCS=str(args.gomaxprocs), GOGC=args.gogc), stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if collector.poll() is not None:
                        raise RuntimeError(f"Collector exited: {output}.log")
                    try:
                        before = metrics()
                        break
                    except Exception:
                        time.sleep(.1)
                else:
                    raise RuntimeError("Collector readiness timed out")
                assert before["otelcol_receiver_accepted_metric_points"] == 0, "use dedicated test ports"
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    task = pool.submit(profile, output, args.profile_seconds) if args.profile_seconds else None
                    run = subprocess.run(
                        ["/usr/bin/time", "-f", "wall=%e user=%U sys=%S maxrss_KiB=%M",
                         str(args.client.resolve()), str(args.batch), str(args.points), str(args.repeats)],
                        env=dict(os.environ, OTEL_EXTERNAL_ENDPOINT="http://127.0.0.1:24317",
                                 OTEL_MAX_IN_FLIGHT=str(inflight), OTEL_CLIENT_CPUS=args.client_cpus), text=True, capture_output=True, timeout=600)
                    print(run.stdout, end="", flush=True)
                    run.check_returncode()
                    after = metrics()
                    raw_metrics=urllib.request.urlopen("http://127.0.0.1:28888/metrics",timeout=5).read()
                    output.with_suffix(".metrics.txt").write_bytes(raw_metrics)
                    if task:
                        task.result()
                delta = {key: after[key] - before[key] for key in before}
                assert delta["otelcol_receiver_accepted_metric_points"] == args.points * args.repeats, delta
                assert delta["otelcol_receiver_refused_metric_points"] == 0, delta
                result = {**vars(args), "inflight": inflight, "stdout": run.stdout,
                          "collector_after": after,
                          "process_time": run.stderr, "collector_delta": delta}
                output.with_suffix(".json").write_text(json.dumps(result, default=str, indent=2))
                print(json.dumps(result, default=str), flush=True)
            finally:
                collector.terminate()
                collector.wait(timeout=20)


if __name__ == "__main__":
    main()
