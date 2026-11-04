#!/usr/bin/env python3
"""Download a GitHub Actions artifact with bounded parallel byte ranges.

Credentials come from the system Git credential helper and are never printed.
This is a fallback for slow GitHub artifact connections; use
fetch_github_artifact.sh for normal downloads.
"""

import argparse
import concurrent.futures
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry


def github_token():
    result = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\n\n",
        text=True,
        capture_output=True,
        check=True,
    )
    for line in result.stdout.splitlines():
        if line.startswith("password="):
            return line.removeprefix("password=")
    raise SystemExit("no GitHub credential in Keychain")


def request_headers(token=None):
    headers = {"Accept": "application/vnd.github+json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return headers


def retrying_session():
    session = requests.Session()
    session.trust_env = False
    retry = Retry(
        total=6,
        connect=6,
        read=6,
        status=6,
        backoff_factor=1,
        status_forcelist=(429, 500, 502, 503, 504),
        allowed_methods=frozenset(("GET", "HEAD")),
    )
    adapter = HTTPAdapter(max_retries=retry)
    session.mount("https://", adapter)
    session.mount("http://", adapter)
    return session


def download_range(url, start, end, output, proxy, retries=4):
    expected = end - start + 1
    command = [
        "curl",
        "-fsSL",
        "--retry",
        str(retries),
        "--retry-all-errors",
        "--retry-delay",
        "2",
        "--max-time",
        "600",
        "-x",
        proxy,
        "--range",
        f"{start}-{end}",
        "-o",
        str(output),
        url,
    ]
    subprocess.run(command, check=True)
    actual = output.stat().st_size
    if actual != expected:
        raise RuntimeError(f"range {start}-{end}: got {actual}, want {expected}")
    return actual


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("run_id", type=int)
    parser.add_argument("artifact_name")
    parser.add_argument("output", type=Path)
    parser.add_argument("--repo", default="jebbranch/ios-port-marble")
    parser.add_argument("--proxy", default="http://127.0.0.1:7890")
    parser.add_argument("--connections", type=int, default=12)
    args = parser.parse_args()

    token = github_token()
    proxies = {"http": args.proxy, "https": args.proxy}
    with retrying_session() as session:
        api = (
            f"https://api.github.com/repos/{args.repo}/actions/runs/"
            f"{args.run_id}/artifacts"
        )
        response = session.get(
            api,
            headers=request_headers(token),
            proxies=proxies,
            timeout=60,
        )
        response.raise_for_status()
        artifact = next(
            (
                item
                for item in response.json().get("artifacts", [])
                if item["name"] == args.artifact_name and not item["expired"]
            ),
            None,
        )
        if artifact is None:
            raise SystemExit("artifact not found or expired")
        archive = f"https://api.github.com/repos/{args.repo}/actions/artifacts/{artifact['id']}/zip"
        response = session.get(
            archive,
            headers=request_headers(token),
            proxies=proxies,
            timeout=60,
            allow_redirects=True,
            stream=True,
        )
        response.raise_for_status()
        url = response.url
        response.close()

        probe = session.get(
            url,
            headers={"Range": "bytes=0-0"},
            proxies=proxies,
            timeout=60,
        )
        probe.raise_for_status()
        content_range = probe.headers.get("Content-Range", "")
        if "/" not in content_range:
            raise SystemExit("artifact server does not support byte ranges")
        total = int(content_range.rsplit("/", 1)[1])

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=args.output.name + ".parts-", dir=args.output.parent
    ) as temp_dir:
        chunk = (total + args.connections - 1) // args.connections
        ranges = [
            (start, min(start + chunk - 1, total - 1))
            for start in range(0, total, chunk)
        ]
        paths = [Path(temp_dir) / f"{index:03d}.part" for index in range(len(ranges))]
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=args.connections
        ) as executor:
            futures = [
                executor.submit(
                    download_range,
                    url,
                    start,
                    end,
                    path,
                    args.proxy,
                )
                for (start, end), path in zip(ranges, paths)
            ]
            for future in concurrent.futures.as_completed(futures):
                future.result()

        temporary = args.output.with_name(args.output.name + ".part")
        with temporary.open("wb") as destination:
            for path in paths:
                with path.open("rb") as source:
                    while block := source.read(1024 * 1024):
                        destination.write(block)
        os.replace(temporary, args.output)
    print(f"{args.output} ({total} bytes)")


if __name__ == "__main__":
    main()
