#!/usr/bin/env python3
"""Recover dropped TCP data after the sender has grown its window."""

import json
from pathlib import Path

from test_tcp_campaign import impaired_run


def main():
    logs = Path("build/tcp-recovery")
    logs.mkdir(parents=True, exist_ok=True)
    result = impaired_run(24, logs, 32768, skip_loss=8)
    if result["fast_retransmits"] < 1:
        raise RuntimeError("no fast retransmission after controlled TCP loss")
    (logs / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[PASS] TCP sliding window and bounded reassembly: {result['fast_retransmits']} fast retransmissions")


if __name__ == "__main__":
    main()
