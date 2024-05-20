"""A reconnectable point-to-point Ethernet link for QEMU test guests."""

from contextlib import ExitStack, contextmanager
import select
import socket
import struct
from threading import Event, Thread


class Relay:
    """Forward framed Ethernet and permit either VM to reconnect after reboot."""

    def __init__(self, listeners, transform):
        self.listeners = listeners
        self.transform = transform
        self.stop = Event()
        self.error = None
        self.accepted = [0, 0]

    def run(self):
        peers = [None, None]
        pending = [bytearray(), bytearray()]

        def close_peer(index):
            if peers[index] is not None:
                peers[index].close()
                peers[index] = None
            pending[index].clear()

        try:
            while not self.stop.is_set():
                ready, _, _ = select.select([*self.listeners, *(p for p in peers if p)], [], [], 0.1)
                for endpoint in ready:
                    if endpoint in self.listeners:
                        index = self.listeners.index(endpoint)
                        close_peer(index)
                        peers[index], _ = endpoint.accept()
                        peers[index].settimeout(2)
                        self.accepted[index] += 1
                        continue
                    if endpoint not in peers:
                        continue
                    index = peers.index(endpoint)
                    try:
                        data = endpoint.recv(65536)
                    except ConnectionError:
                        data = b""
                    if not data:
                        close_peer(index)
                        continue
                    buffer = pending[index]
                    buffer.extend(data)
                    while len(buffer) >= 4:
                        size, = struct.unpack_from("!I", buffer)
                        if not 14 <= size <= 1518:
                            raise RuntimeError(f"invalid Ethernet frame length {size}")
                        if len(buffer) < size + 4:
                            break
                        packet = bytes(buffer[:size + 4])
                        del buffer[:size + 4]
                        target = 1 - index
                        if peers[target] is not None:
                            try:
                                for forwarded in self.transform(index, packet):
                                    peers[target].sendall(forwarded)
                            except ConnectionError:
                                close_peer(target)
        except Exception as error:
            self.error = error
        finally:
            for index in range(2):
                close_peer(index)


@contextmanager
def link(transform=None):
    with ExitStack() as stack:
        listeners = [stack.enter_context(socket.socket()) for _ in range(2)]
        for listener in listeners:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
        relay = Relay(listeners, transform or (lambda _, packet: (packet,)))
        worker = Thread(target=relay.run, daemon=True)
        worker.start()
        try:
            yield relay, [listener.getsockname()[1] for listener in listeners]
        finally:
            relay.stop.set()
            worker.join(timeout=5)
            if worker.is_alive() or relay.error:
                raise RuntimeError(f"Ethernet relay failed: {relay.error}")
