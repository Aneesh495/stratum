"""JSON command transport with a retained QEMU monitor transcript."""

from contextlib import contextmanager
import json
import socket
import time


@contextmanager
def monitor(path, log):
    with socket.socket(socket.AF_UNIX) as sock:
        deadline = time.monotonic() + 10
        while True:
            try:
                sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        sock.settimeout(20)
        with sock.makefile("rb") as reader, log.open("w") as transcript:
            def receive():
                raw = reader.readline()
                if not raw:
                    raise RuntimeError("QMP monitor disconnected")
                transcript.write(raw.decode("ascii"))
                transcript.flush()
                return json.loads(raw)

            def execute(name, arguments=None):
                request = {"execute": name}
                if arguments is not None:
                    request["arguments"] = arguments
                command_line = json.dumps(request) + "\n"
                transcript.write(command_line)
                sock.sendall(command_line.encode("ascii"))
                while True:
                    answer = receive()
                    if "error" in answer:
                        raise RuntimeError(f"QMP {name} failed: {answer['error']}")
                    if "return" in answer:
                        return answer["return"]

            if "QMP" not in receive():
                raise RuntimeError("expected QMP hello")
            execute("qmp_capabilities")
            yield execute
