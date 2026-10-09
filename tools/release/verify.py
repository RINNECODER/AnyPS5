#!/usr/bin/env python3
"""Independently verify a signed AnyPS5 channel feed using only the public key.

    python3 verify.py https://github.com/RINNECODER/AnyPS5/releases/download/channels/engine-alpha.json

Self-contained and stdlib-only on purpose: copy this one file anywhere and run it.
It follows the client verification order of the release contract:

1. fetch the feed and ``<feed>.sig``; verify the Ed25519 signature with the embedded key;
2. check schema, product, channel (from the feed name) and ``--min-sequence``;
3. fetch the build release's ``release.json`` + ``.sig``, verify it, and require it to
   equal the feed's ``release`` object;
4. unless ``--no-download``, download the asset and check its size and SHA-256;
5. with ``--extract DIR``, safely unzip it and check the package manifest SHA-256.

Exit status is 0 only when every requested check passed. The Ed25519 code is a direct
transcription of RFC 8032 section 6 (verification only); it never sees a private key.
"""
import argparse
import base64
import binascii
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tempfile
import urllib.request
import zipfile

PUBLIC_KEY_B64 = "8CH+bArwdc+38zujG9xU/pD1W7tRoi5UAeGSschuCpc="
REPO = "RINNECODER/AnyPS5"
DOWNLOAD_BASE = "https://github.com/" + REPO + "/releases/download/"
FEED_NAME = re.compile(r"(engine|macps)-(alpha|beta|stable)\.json")
VERSION = re.compile(r"[0-9]{4}\.[0-9]{2}\.[0-9]{2}\.[1-9][0-9]*")
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")
RFC3339_UTC = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?Z")
MAX_JSON_BYTES = 1 << 20
MAX_ASSET_BYTES = 2 << 30
USER_AGENT = "anyps5-release-verify/1"

# ---- Ed25519 verification (RFC 8032, section 6) ---------------------------------------
_P = 2 ** 255 - 19
_Q = 2 ** 252 + 27742317777372353535851937790883648493


def _inv(x):
    return pow(x, _P - 2, _P)


_D = -121665 * _inv(121666) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _add(a, b):
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    e1, f1 = (y1 - x1) * (y2 - x2) % _P, (y1 + x1) * (y2 + x2) % _P
    c, d = 2 * t1 * t2 * _D % _P, 2 * z1 * z2 % _P
    e, f, g, h = f1 - e1, d - c, d + c, f1 + e1
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(scalar, point):
    result = (0, 1, 1, 0)
    while scalar > 0:
        if scalar & 1:
            result = _add(result, point)
        point = _add(point, point)
        scalar >>= 1
    return result


def _equal(a, b):
    return (a[0] * b[2] - b[0] * a[2]) % _P == 0 and (a[1] * b[2] - b[1] * a[2]) % _P == 0


def _recover_x(y, sign):
    if y >= _P:
        return None
    x2 = (y * y - 1) * _inv(_D * y * y + 1) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


def _decompress(data):
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign, y = y >> 255, y & ((1 << 255) - 1)
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % _P)


_GY = 4 * _inv(5) % _P
_G = (_recover_x(_GY, 0), _GY, 1, _recover_x(_GY, 0) * _GY % _P)


def ed25519_verify(public_key, message, signature):
    """Return True only for a valid RFC 8032 Ed25519 signature."""
    if len(public_key) != 32 or len(signature) != 64:
        return False
    a = _decompress(public_key)
    r = _decompress(signature[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= _Q:
        return False
    h = int.from_bytes(hashlib.sha512(signature[:32] + public_key + message).digest(), "little") % _Q
    return _equal(_mul(s, _G), _add(r, _mul(h, a)))


# ---- signed documents ------------------------------------------------------------------
class VerificationError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise VerificationError(message)


def public_key():
    return base64.b64decode(PUBLIC_KEY_B64, validate=True)


def decode_signature(text):
    """A .sig file is base64 of the 64-byte signature plus one newline."""
    require(isinstance(text, (bytes, bytearray)) and text.endswith(b"\n") and text.count(b"\n") == 1,
            "signature file must be one base64 line ending in a newline")
    try:
        raw = base64.b64decode(text[:-1], validate=True)
    except (binascii.Error, ValueError):
        raise VerificationError("signature file is not valid base64") from None
    require(len(raw) == 64, "signature must decode to 64 bytes")
    return raw


def verify_signed(data, signature_text, key=None):
    """Verify the exact bytes ``data`` against ``<name>.sig`` contents."""
    signature = decode_signature(signature_text)
    require(ed25519_verify(key if key is not None else public_key(), bytes(data), signature),
            "Ed25519 signature does not verify with the release public key")


def _unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def parse_json(data):
    require(len(data) <= MAX_JSON_BYTES, "signed JSON document is too large")
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=_unique)
    except (UnicodeDecodeError, ValueError) as error:
        raise VerificationError("signed document is not valid JSON: " + str(error)) from None
    require(isinstance(value, dict), "signed document must be a JSON object")
    return value


def _integer(value):
    return type(value) is int


def asset_name(product, version):
    return ("anyps5-engine-" if product == "engine" else "MacPS-") + version + "-macos-arm64.zip"


def check_release(record, product=None):
    """Structural checks for a schema-1 ``release.json`` object."""
    require(isinstance(record, dict), "release record must be an object")
    require(record.get("schema") == 1 and _integer(record.get("schema")), "release schema must be 1")
    require(record.get("product") in ("engine", "macps"), "unknown release product")
    require(product is None or record["product"] == product, "release product does not match the feed")
    version = record.get("version")
    require(isinstance(version, str) and VERSION.fullmatch(version) is not None, "malformed release version")
    require(record.get("channel_at_build") in ("alpha", "beta", "stable"), "malformed channel_at_build")
    require(isinstance(record.get("commit"), str) and HEX40.fullmatch(record["commit"]) is not None,
            "release commit must be 40 lowercase hex characters")
    require(isinstance(record.get("built_at"), str) and RFC3339_UTC.fullmatch(record["built_at"]) is not None,
            "built_at must be an RFC3339 UTC timestamp")
    require(record.get("min_macps") is None or isinstance(record.get("min_macps"), str), "malformed min_macps")
    asset = record.get("asset")
    require(isinstance(asset, dict), "release asset must be an object")
    require(asset.get("name") == asset_name(record["product"], version), "unexpected asset name")
    if record["product"] == "engine":
        tag = "engine-" + version
        require(asset.get("url") == DOWNLOAD_BASE + tag + "/" + asset["name"],
                "engine asset URL is not the immutable build release download")
    else:
        require(isinstance(asset.get("url"), str) and asset["url"].startswith("https://"), "malformed asset URL")
    require(isinstance(asset.get("sha256"), str) and HEX64.fullmatch(asset["sha256"]) is not None,
            "asset sha256 must be 64 lowercase hex characters")
    require(_integer(asset.get("size")) and 0 < asset["size"] <= MAX_ASSET_BYTES, "asset size out of range")
    manifest = record.get("package_manifest_sha256")
    require(record["product"] != "engine" or (isinstance(manifest, str) and HEX64.fullmatch(manifest) is not None),
            "package_manifest_sha256 must be 64 lowercase hex characters")
    require(isinstance(record.get("notes"), str), "notes must be a string")
    return record


def check_feed(feed, product, channel, min_sequence=0):
    require(feed.get("schema") == 1 and _integer(feed.get("schema")), "feed schema must be 1")
    require(feed.get("product") == product, "feed product does not match its name")
    require(feed.get("channel") == channel, "feed channel does not match its name")
    sequence = feed.get("sequence")
    require(_integer(sequence) and sequence >= 1, "feed sequence must be a positive integer")
    require(sequence >= min_sequence, "feed sequence %d is lower than the last accepted %d" % (sequence, min_sequence))
    require(isinstance(feed.get("updated_at"), str) and RFC3339_UTC.fullmatch(feed["updated_at"]) is not None,
            "updated_at must be an RFC3339 UTC timestamp")
    check_release(feed.get("release"), product)
    return feed


def verify_feed_bytes(name, data, signature_text, min_sequence=0, key=None):
    """Signature first, then structure. Returns the parsed feed."""
    match = FEED_NAME.fullmatch(name)
    require(match is not None, "unexpected feed name: " + name)
    verify_signed(data, signature_text, key)
    return check_feed(parse_json(data), match.group(1), match.group(2), min_sequence)


# ---- network and archive ---------------------------------------------------------------
def fetch(url, limit=MAX_JSON_BYTES):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Cache-Control": "no-cache"})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(limit + 1)
    require(len(data) <= limit, "download exceeds its size limit: " + url)
    return data


def download(url, target, size, sha256):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    digest, total = hashlib.sha256(), 0
    with urllib.request.urlopen(request, timeout=120) as response, open(target, "wb") as out:
        while True:
            chunk = response.read(1 << 20)
            if not chunk:
                break
            total += len(chunk)
            require(total <= size, "asset is larger than its signed size")
            digest.update(chunk)
            out.write(chunk)
    require(total == size, "asset size %d differs from signed size %d" % (total, size))
    require(digest.hexdigest() == sha256, "asset SHA-256 differs from the signed value")


def safe_extract(archive, destination):
    """Unzip without absolute paths, '..', symlinks or duplicate entries; restore exec bits."""
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    seen = set()
    with zipfile.ZipFile(archive) as bundle:
        for info in bundle.infolist():
            name = info.filename
            path = PurePosixPath(name)
            require(name and not name.startswith("/") and "\\" not in name and "\0" not in name and
                    all(part not in ("", ".", "..") for part in path.parts), "unsafe archive path: " + name)
            mode = info.external_attr >> 16
            require(not stat.S_ISLNK(mode), "archive contains a symbolic link: " + name)
            key = name.rstrip("/")
            require(key not in seen, "duplicate archive entry: " + name)
            seen.add(key)
            target = destination.joinpath(*path.parts)
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with bundle.open(info) as source, open(target, "xb") as out:
                while True:
                    chunk = source.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
            os.chmod(target, 0o755 if mode & 0o111 else 0o644)
    return destination


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("feed_url")
    parser.add_argument("--min-sequence", type=int, default=0, help="last sequence this client accepted")
    parser.add_argument("--no-download", action="store_true", help="skip downloading the package asset")
    parser.add_argument("--extract", type=Path, help="new directory to unzip the verified package into")
    args = parser.parse_args(argv)
    name = args.feed_url.rstrip("/").rsplit("/", 1)[-1]
    try:
        feed = verify_feed_bytes(name, fetch(args.feed_url), fetch(args.feed_url + ".sig", 4096), args.min_sequence)
        release = feed["release"]
        summary = {"feed": name, "signature": "valid", "product": feed["product"], "channel": feed["channel"],
                   "sequence": feed["sequence"], "updated_at": feed["updated_at"], "version": release["version"],
                   "commit": release["commit"], "asset": release["asset"]["name"], "asset_sha256": release["asset"]["sha256"],
                   "asset_size": release["asset"]["size"]}
        if feed["product"] == "engine":
            base = DOWNLOAD_BASE + "engine-" + release["version"] + "/release.json"
            record_bytes = fetch(base)
            verify_signed(record_bytes, fetch(base + ".sig", 4096))
            require(check_release(parse_json(record_bytes), "engine") == release,
                    "feed release object differs from the signed build release.json")
            summary["release_json"] = "valid and identical to the feed"
        if not args.no_download or args.extract:
            with tempfile.TemporaryDirectory(prefix="anyps5-verify-") as scratch:
                archive = Path(scratch) / release["asset"]["name"]
                download(release["asset"]["url"], archive, release["asset"]["size"], release["asset"]["sha256"])
                summary["asset_download"] = "size and sha256 match"
                if args.extract:
                    root = safe_extract(archive, args.extract)
                    if feed["product"] == "engine":
                        require(sha256_file(root / "manifest.json") == release["package_manifest_sha256"],
                                "extracted manifest SHA-256 differs from package_manifest_sha256")
                        summary["package_manifest"] = "matches"
                    summary["extracted_to"] = str(root)
    except (VerificationError, OSError, KeyError) as error:
        print(json.dumps({"feed": name, "status": "REJECTED", "reason": str(error)}), file=sys.stderr)
        return 1
    summary["status"] = "VERIFIED"
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
