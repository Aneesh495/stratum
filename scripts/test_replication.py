#!/usr/bin/env python3
"""Transfer one durable record between two Stratum guests on an isolated Ethernet link."""

from contextlib import contextmanager
from pathlib import Path
import select
import socket
import struct
import tempfile
from threading import Event, Thread

from test_smoke import PROMPT, require, shell
from vm import BOOT_IMAGE, boot_profile, command, fresh_disk, running


def check_capture(capture, request, response, *, require_retry=True):
    data = capture.read_bytes()
    endian = {b"\xd4\xc3\xb2\xa1": "<", b"\xa1\xb2\xc3\xd4": ">"}.get(data[:4])
    if endian is None or struct.unpack_from(endian + "I", data, 20)[0] != 1:
        raise RuntimeError("expected Ethernet pcap")
    records = {}
    arp_seen = False
    retransmitted = False
    offset = 24
    while offset < len(data):
        _, _, captured, original = struct.unpack_from(endian + "4I", data, offset)
        offset += 16
        frame = data[offset:offset + captured]
        offset += captured
        if len(frame) != captured or captured != original:
            raise RuntimeError("truncated Ethernet capture")
        if frame[12:14] == b"\x08\x06":
            arp_seen = True
        if frame[12:14] != b"\x08\x00" or len(frame) < 54 or frame[23] != 6:
            continue
        ip_header = (frame[14] & 15) * 4
        end = 14 + int.from_bytes(frame[16:18], "big")
        segment = frame[14 + ip_header:end]
        src, dst = socket.inet_ntoa(frame[26:30]), socket.inet_ntoa(frame[30:34])
        sport, dport = struct.unpack_from("!HH", segment)
        payload = segment[(segment[12] >> 4) * 4:]
        if 9090 in (sport, dport) and payload:
            stream = records.setdefault((src, dst), {})
            sequence = segment[4:8]
            if stream.get(sequence) == response and payload == response:
                retransmitted = True
            stream[sequence] = payload
    if not arp_seen:
        raise RuntimeError("capture lacks ARP discovery")
    if require_retry and not retransmitted:
        raise RuntimeError("capture lacks a retransmitted reply")
    for flow, expected in ((("10.0.2.16", "10.0.2.15"), request),
                           (("10.0.2.15", "10.0.2.16"), response)):
        chunks = records.get(flow, {})
        stream = b"".join(chunks[seq] for seq in sorted(chunks))
        if stream != expected:
            raise RuntimeError(f"unexpected Ethernet flow {flow}: {stream!r}")


class EthernetRelay:
    """Forward QEMU socket-netdev frames and drop the first ledger reply."""

    def __init__(self, response):
        self.response = response
        self.dropped = False
        self.error = None
        self.stop = Event()
        self.listeners = []
        self.peers = []

    def serve(self):
        try:
            for listener in self.listeners:
                while not self.stop.is_set():
                    if select.select([listener], [], [], 0.1)[0]:
                        peer, _ = listener.accept()
                        peer.settimeout(2)
                        self.peers.append(peer)
                        break
                if self.stop.is_set():
                    return

            pending = {peer: bytearray() for peer in self.peers}
            while not self.stop.is_set():
                for peer in select.select(self.peers, [], [], 0.1)[0]:
                    chunk = peer.recv(65536)
                    if not chunk:
                        return
                    pending[peer].extend(chunk)
                    while len(pending[peer]) >= 4:
                        size = struct.unpack_from("!I", pending[peer])[0]
                        if not 14 <= size <= 1518:
                            raise RuntimeError(f"invalid QEMU Ethernet frame size {size}")
                        end = size + 4
                        if len(pending[peer]) < end:
                            break
                        frame = bytes(pending[peer][:end])
                        del pending[peer][:end]
                        if peer is self.peers[0] and not self.dropped and frame.endswith(self.response):
                            self.dropped = True
                        else:
                            destination = self.peers[1] if peer is self.peers[0] else self.peers[0]
                            destination.sendall(frame)
        except OSError as error:
            if not self.stop.is_set():
                self.error = error
        except Exception as error:
            self.error = error
        finally:
            for peer in self.peers:
                peer.close()


@contextmanager
def impaired_link(response):
    relay = EthernetRelay(response)
    try:
        for _ in range(2):
            listener = socket.socket()
            relay.listeners.append(listener)
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
        thread = Thread(target=relay.serve)
        thread.start()
        try:
            yield relay, tuple(listener.getsockname()[1] for listener in relay.listeners)
        finally:
            relay.stop.set()
            thread.join(timeout=4)
            if thread.is_alive():
                raise RuntimeError("Ethernet relay failed to shut down")
    finally:
        for listener in relay.listeners:
            listener.close()


def main():
    log = Path("build/replication")
    log.mkdir(parents=True, exist_ok=True)
    capture = log / "ethernet.pcap"
    response = b"OK via-ethernet\n"
    with fresh_disk() as primary_disk, fresh_disk() as follower_disk, tempfile.TemporaryDirectory(dir="build") as directory:
        follower_boot = boot_profile(BOOT_IMAGE, Path(directory), "ip=10.0.2.16")
        with impaired_link(response) as (relay, (source_port, follower_port)):
            primary = command(BOOT_IMAGE, primary_disk, cpus=2, ethernet=f"connect=127.0.0.1:{source_port}",
                              capture=capture)
            follower = command(follower_boot, follower_disk, cpus=2, ethernet=f"connect=127.0.0.1:{follower_port}",
                               mac="52:54:00:12:34:57")
            with running(primary, log / "primary.log") as source:
                source.expect(PROMPT)
                require(b"Ledger record durable.", shell(source, "ledgerput peer-1 via-ethernet"))
                source.send(b"ledgerd\r")
                source.expect("ledgerd listening on port 9090.")
                with running(follower, log / "follower.log") as guest:
                    guest.expect(PROMPT)
                    require(b"Ledger record durable.", shell(guest, "ledgerpull 10.0.2.15 peer-1"))
                    if shell(guest, "cat /ledger/peer-1") != b"via-ethernet":
                        raise RuntimeError("follower stored incorrect bytes")
                    guest.interrupt()
        if relay.error or not relay.dropped:
            raise RuntimeError(f"Ethernet impairment did not complete: {relay.error}")
        check_capture(capture, b"GET peer-1\n", response)
        with running(command(BOOT_IMAGE, follower_disk, cpus=2), log / "follower-reboot.log") as guest:
            guest.expect(PROMPT)
            if shell(guest, "cat /ledger/peer-1") != b"via-ethernet":
                raise RuntimeError("follower record did not survive reboot")

    print("[PASS] Two guests recovered a dropped TCP reply, verified wire bytes, and persisted the record")


if __name__ == "__main__":
    main()
