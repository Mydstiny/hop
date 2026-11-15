"""Write disposable loopback test identities a, b, bad and c.

They come from the product TLS runtime, so they have exactly the format a real
device identity has. Use them only for loopback tests, never on a device.

    python3 make_tls_fixtures.py /abs/libhop_tls_runtime.so /abs/new-directory
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hop_linux.tls import TLSLibrary  # noqa: E402


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    library, out = TLSLibrary(sys.argv[1]), Path(sys.argv[2])
    if not out.is_absolute():
        raise SystemExit('output directory must be absolute')
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    for name in ('a', 'b', 'bad', 'c'):
        certificate, key = library.generate_identity()
        (out / (name + '.crt')).write_bytes(certificate)
        fd = os.open(out / (name + '.key'), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            os.write(fd, key)
        finally:
            os.close(fd)
    print('wrote disposable test identities to', out)


if __name__ == '__main__':
    main()
