"""Run with printer services unreachable to check HTTP across reconnect attempts.

Usage: python3 tools/check-webui.py http://10.60.2.108
Does not change gateway settings. Run again with the printer reachable.
"""

import sys
import time
import urllib.request


def main():
    base = sys.argv[1].rstrip("/")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    for path in ("/", "/config/wifi", "/config/settings") * 3:
        start = time.monotonic()
        with opener.open(base + path, timeout=15) as response:
            assert response.status == 200, response.status
            assert b"BambuTagger" in response.read(), "Incomplete or unexpected page"
        elapsed = time.monotonic() - start
        assert elapsed < 15, f"HTTP stalled for {elapsed:.1f}s"
        print(f"PASS {path}: {elapsed:.2f}s", flush=True)
        time.sleep(2)


if __name__ == "__main__":
    main()
