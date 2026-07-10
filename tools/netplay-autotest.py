#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path


DESYNC_MARKER = "Netplay: DESYNC DETECTED"
HOST_READY_MARKER = "Netplay autotest: host listening"
GAME_START_MARKER = "Netplay autotest: client ready, starting game"


def build_command(args, role, seed):
    cmd = [
        str(args.melon),
        f"--netplay-autotest={role}",
        "--netplay-autotest-host",
        args.host,
        "--netplay-autotest-port",
        str(args.port),
        "--netplay-autotest-seed",
        str(seed),
        "--netplay-autotest-duration",
        str(args.duration),
        str(args.rom),
    ]

    stdbuf = shutil.which("stdbuf")
    if stdbuf:
        return [stdbuf, "-oL", "-eL", *cmd]
    return cmd


def terminate_process(proc, grace=5.0):
    if proc.poll() is not None:
        return

    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return

    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return
        time.sleep(0.05)

    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def main():
    parser = argparse.ArgumentParser(description="Run an unattended two-process melonDS netplay smoke test.")
    parser.add_argument("--melon", required=True, type=Path, help="Path to melonDS executable")
    parser.add_argument("--rom", required=True, type=Path, help="Path to NDS ROM")
    parser.add_argument("--duration", type=int, default=300, help="Run duration in seconds")
    parser.add_argument("--seed", type=int, default=1, help="Deterministic input seed")
    parser.add_argument("--host", default="127.0.0.1", help="Host address for the client")
    parser.add_argument("--port", type=int, default=8064, help="Netplay port")
    parser.add_argument("--connect-timeout", type=int, default=30, help="Seconds to wait for startup/game start")
    parser.add_argument("--artifact-dir", type=Path, default=Path("netplay-runs"), help="Directory for run artifacts")
    parser.add_argument("--no-offscreen", action="store_true", help="Do not set QT_QPA_PLATFORM=offscreen")
    args = parser.parse_args()

    args.melon = args.melon.resolve()
    args.rom = args.rom.resolve()
    if not args.melon.exists():
        print(f"melonDS executable not found: {args.melon}", file=sys.stderr)
        return 2
    if not args.rom.exists():
        print(f"ROM not found: {args.rom}", file=sys.stderr)
        return 2

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = args.artifact_dir / f"{stamp}-seed-{args.seed}-port-{args.port}"
    run_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    if not args.no_offscreen:
        env.setdefault("QT_QPA_PLATFORM", "offscreen")

    events = []
    events_lock = threading.Lock()
    stop_readers = threading.Event()

    def record(event):
        with events_lock:
            events.append({"time": time.time(), **event})

    def reader(name, pipe, log_path):
        with log_path.open("w", encoding="utf-8", errors="replace") as log:
            for line in iter(pipe.readline, ""):
                log.write(line)
                log.flush()
                if DESYNC_MARKER in line:
                    record({"kind": "desync", "process": name, "line": line.rstrip()})
                elif HOST_READY_MARKER in line:
                    record({"kind": "host-ready", "process": name, "line": line.rstrip()})
                elif GAME_START_MARKER in line or "starting netplay game" in line:
                    record({"kind": "game-start", "process": name, "line": line.rstrip()})
                if stop_readers.is_set():
                    break

    host_cmd = build_command(args, "host", args.seed)
    client_cmd = build_command(args, "client", args.seed)

    processes = {}
    threads = []
    start_time = time.monotonic()
    failure = None

    try:
        record({"kind": "launch", "process": "host", "cmd": host_cmd})
        host_proc = subprocess.Popen(
            host_cmd,
            cwd=run_dir,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            start_new_session=True,
        )
        processes["host"] = host_proc
        t = threading.Thread(target=reader, args=("host", host_proc.stdout, run_dir / "host.log"), daemon=True)
        t.start()
        threads.append(t)

        deadline = time.monotonic() + args.connect_timeout
        while time.monotonic() < deadline:
            if host_proc.poll() is not None:
                failure = f"host exited before client launch with code {host_proc.returncode}"
                break
            with events_lock:
                if any(e["kind"] == "host-ready" for e in events):
                    break
            time.sleep(0.05)
        else:
            failure = "timed out waiting for host to listen"

        if not failure:
            record({"kind": "launch", "process": "client", "cmd": client_cmd})
            client_proc = subprocess.Popen(
                client_cmd,
                cwd=run_dir,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                start_new_session=True,
            )
            processes["client"] = client_proc
            t = threading.Thread(target=reader, args=("client", client_proc.stdout, run_dir / "client.log"), daemon=True)
            t.start()
            threads.append(t)

            deadline = time.monotonic() + args.connect_timeout
            while time.monotonic() < deadline:
                with events_lock:
                    if any(e["kind"] == "desync" for e in events):
                        failure = "desync detected during startup"
                        break
                    if any(e["kind"] == "game-start" for e in events):
                        break

                for name, proc in processes.items():
                    if proc.poll() is not None:
                        failure = f"{name} exited during startup with code {proc.returncode}"
                        break
                if failure:
                    break
                time.sleep(0.05)
            else:
                failure = "timed out waiting for game start"

        end_time = start_time + args.duration + args.connect_timeout
        while not failure and time.monotonic() < end_time:
            with events_lock:
                desync = next((e for e in events if e["kind"] == "desync"), None)
            if desync:
                failure = desync["line"]
                break

            all_exited = True
            for name, proc in processes.items():
                code = proc.poll()
                if code is None:
                    all_exited = False
                elif code != 0:
                    failure = f"{name} exited with code {code}"
                    break
            if failure or all_exited:
                break

            time.sleep(0.1)

        if not failure:
            for name, proc in processes.items():
                code = proc.poll()
                if code not in (0, None):
                    failure = f"{name} exited with code {code}"
                    break

    finally:
        for proc in processes.values():
            terminate_process(proc)
        stop_readers.set()
        for t in threads:
            t.join(timeout=2)

    for dump in run_dir.glob("netplay-desync-*.mln"):
        dump.touch()

    summary = {
        "result": "fail" if failure else "pass",
        "failure": failure,
        "melon": str(args.melon),
        "rom": str(args.rom),
        "duration": args.duration,
        "seed": args.seed,
        "port": args.port,
        "qt_qpa_platform": env.get("QT_QPA_PLATFORM"),
        "processes": {name: proc.returncode for name, proc in processes.items()},
        "events": events,
    }
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

    print(f"netplay autotest {'FAILED' if failure else 'passed'}: {run_dir}")
    if failure:
        print(f"failure: {failure}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
