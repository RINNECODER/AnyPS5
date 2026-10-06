import math
import subprocess
import sys
import zlib


runner, fixture = sys.argv[1:]
pixels = bytes(((i * 37 + (i >> 3)) ^ (i >> 5)) & 255 for i in range(4096))
crc = zlib.crc32(pixels)
for limit in (2, 31, 1000, 4095):
    primes = [n for n in range(2, limit + 1)
              if all(n % divisor for divisor in range(2, math.isqrt(n) + 1))]
    expected = (f"homebrew primes={len(primes)} sum={sum(primes)} "
                f"buffer_crc32={crc} tls=ok bss=ok\n")
    result = subprocess.run([runner, fixture, str(limit)], capture_output=True,
                            text=True, timeout=20)
    assert result.returncode == 0, (result.returncode, result.stderr)
    assert result.stdout == expected, (limit, result.stdout, expected)
    assert "guest_exit=0" in result.stderr, result.stderr
    print(f"limit={limit}: {result.stdout.strip()}")

for argument in ("1", "4096", "abc"):
    result = subprocess.run([runner, fixture, argument], capture_output=True,
                            text=True, timeout=20)
    assert result.returncode == 2, (argument, result.returncode, result.stderr)
    assert result.stdout == "", result.stdout
    assert "guest_exit=2" in result.stderr, result.stderr
print("guest argument rejection and exit propagation: PASS")
