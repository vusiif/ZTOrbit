#!/usr/bin/env python3
"""Build, sign and verify Z2 automatic-update manifests.

The client verifies a detached Ed25519 signature over the *exact bytes* of the
manifest file as served. This tool therefore signs the serialized manifest byte
string and writes it next to the manifest as <manifest>.sig (standard Base64,
newline-terminated). No re-serialization is involved on the client, so the
canonicalization rule is simply: "signature covers manifest.json as served".

Examples (run from the repository root):

  # 1. One-time signing key. Keep the seed private; publish only the .pub file.
  python scripts/update-manifest.py keygen --out-dir build/update-keys

  # 2. Create the package manifest and its detached signature.
  python scripts/update-manifest.py generate \
      --package build/dist/Z2-windows-x64.zip \
      --version 0.5.4 --platform windows --arch x64 \
      --url 'http://10.10.10.1:18102/file?version=0.5.4' \
      --min-updatable-version 0.5.0 \
      --private-key build/update-keys/update-signing.key \
      --out-dir build/dist/update/stable/windows-x64

  # 3. Verify what a client will verify (same bytes, same key).
  python scripts/update-manifest.py verify \
      --manifest build/dist/update/stable/windows-x64/manifest.json \
      --signature build/dist/update/stable/windows-x64/manifest.json.sig \
      --public-key build/update-keys/update-signing.pub

PyNaCl is used when available. Without it the script falls back to a small,
self-contained RFC 8032 Ed25519 implementation so packaging works on a plain
Python 3 installation.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import secrets
import sys

# ---------------------------------------------------------------------------
# Ed25519 (RFC 8032). A small fallback implementation keeps this tool free of
# mandatory third-party packages; PyNaCl is preferred when installed.
# ---------------------------------------------------------------------------

_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _P - 2, _P)) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _sha512(data: bytes) -> bytes:
    return hashlib.sha512(data).digest()


def _xrecover(y: int) -> int:
    xx = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P)
    x = pow(xx, (_P + 3) // 8, _P)
    if (x * x - xx) % _P != 0:
        x = (x * _I) % _P
    if x % 2 != 0:
        x = _P - x
    return x


def _edwards_add(p, q):
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = ((y1 - x1) * (y2 - x2)) % _P
    b = ((y1 + x1) * (y2 + x2)) % _P
    c = (t1 * 2 * _D * t2) % _P
    d = (z1 * 2 * z2) % _P
    e = (b - a) % _P
    f = (d - c) % _P
    g = (d + c) % _P
    h = (b + a) % _P
    return ((e * f) % _P, (g * h) % _P, (f * g) % _P, (e * h) % _P)


def _scalarmult(point, scalar: int):
    result = (0, 1, 1, 0)
    while scalar > 0:
        if scalar & 1:
            result = _edwards_add(result, point)
        point = _edwards_add(point, point)
        scalar >>= 1
    return result


def _encodepoint(point) -> bytes:
    x, y, z, _ = point
    zinv = pow(z, _P - 2, _P)
    x = (x * zinv) % _P
    y = (y * zinv) % _P
    value = y | ((x & 1) << 255)
    return value.to_bytes(32, "little")


def _decodepoint(data: bytes):
    if len(data) != 32:
        raise ValueError("point must be 32 bytes")
    value = int.from_bytes(data, "little")
    sign = value >> 255
    y = value & ((1 << 255) - 1)
    if y >= _P:
        raise ValueError("invalid point")
    x = _xrecover(y)
    if x & 1 != sign:
        x = _P - x
    point = (x, y, 1, (x * y) % _P)
    if not _is_on_curve(point):
        raise ValueError("point is not on the curve")
    return point


def _is_on_curve(point) -> bool:
    x, y, z, t = point
    return ((-x * x + y * y) * z * z - z**4 - _D * t * t) % _P == 0


_BY = (4 * pow(5, _P - 2, _P)) % _P
_BX = _xrecover(_BY)
_B = (_BX, _BY, 1, (_BX * _BY) % _P)


def _expand_seed(seed: bytes):
    digest = _sha512(seed)
    scalar = int.from_bytes(digest[:32], "little")
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    return scalar, digest[32:]


def ed25519_public_key(seed: bytes) -> bytes:
    scalar, _ = _expand_seed(seed)
    return _encodepoint(_scalarmult(_B, scalar))


def ed25519_sign(seed: bytes, message: bytes) -> bytes:
    scalar, prefix = _expand_seed(seed)
    public = _encodepoint(_scalarmult(_B, scalar))
    nonce = int.from_bytes(_sha512(prefix + message), "little") % _L
    encoded_r = _encodepoint(_scalarmult(_B, nonce))
    challenge = int.from_bytes(_sha512(encoded_r + public + message), "little") % _L
    signature = (nonce + challenge * scalar) % _L
    return encoded_r + signature.to_bytes(32, "little")


def ed25519_verify(public: bytes, message: bytes, signature: bytes) -> bool:
    if len(public) != 32 or len(signature) != 64:
        return False
    try:
        point_a = _decodepoint(public)
        point_r = _decodepoint(signature[:32])
    except ValueError:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= _L:
        return False
    challenge = int.from_bytes(_sha512(signature[:32] + public + message), "little") % _L
    left = _scalarmult(_B, s)
    right = _edwards_add(point_r, _scalarmult(point_a, challenge))
    return _encodepoint(left) == _encodepoint(right)


try:  # pragma: no cover - depends on the developer machine
    import nacl.signing as _nacl_signing  # type: ignore

    def sign_seed(seed: bytes, message: bytes) -> bytes:  # noqa: F811
        return _nacl_signing.SigningKey(seed).sign(message).signature

    def public_from_seed(seed: bytes) -> bytes:  # noqa: F811
        return bytes(_nacl_signing.SigningKey(seed).verify_key)

    def verify_detached(public: bytes, message: bytes, signature: bytes) -> bool:  # noqa: F811
        try:
            _nacl_signing.VerifyKey(public).verify(message, signature)
            return True
        except Exception:
            return False

    BACKEND = "PyNaCl"
except Exception:  # pragma: no cover
    sign_seed = ed25519_sign
    public_from_seed = ed25519_public_key
    verify_detached = ed25519_verify
    BACKEND = "built-in RFC 8032"


# ---------------------------------------------------------------------------
# Key handling
# ---------------------------------------------------------------------------

SEED_BYTES = 32
PUBLIC_BYTES = 32
SIGNATURE_BYTES = 64


def read_seed(path: Path) -> bytes:
    text = path.read_text(encoding="ascii").strip()
    try:
        seed = base64.b64decode(text, validate=True)
    except Exception as exc:  # noqa: BLE001
        raise SystemExit(f"private key is not valid Base64: {path}: {exc}")
    if len(seed) != SEED_BYTES:
        raise SystemExit(f"private key must encode {SEED_BYTES} bytes: {path}")
    return seed


def read_public(path: Path) -> bytes:
    text = path.read_text(encoding="ascii").strip()
    try:
        public = base64.b64decode(text, validate=True)
    except Exception as exc:  # noqa: BLE001
        raise SystemExit(f"public key is not valid Base64: {path}: {exc}")
    if len(public) != PUBLIC_BYTES:
        raise SystemExit(f"public key must encode {PUBLIC_BYTES} bytes: {path}")
    return public


def write_private(path: Path, seed: bytes) -> None:
    path.write_text(base64.b64encode(seed).decode("ascii") + "\n", encoding="ascii")
    try:
        path.chmod(0o600)
    except OSError:
        pass


def write_public(path: Path, public: bytes) -> None:
    path.write_text(base64.b64encode(public).decode("ascii") + "\n", encoding="ascii")


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def command_keygen(args) -> int:
    out = Path(args.out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    private_path = out / "update-signing.key"
    public_path = out / "update-signing.pub"
    if private_path.exists() and not args.force:
        raise SystemExit(f"refusing to overwrite an existing signing key: {private_path}")
    seed = secrets.token_bytes(SEED_BYTES)
    write_private(private_path, seed)
    write_public(public_path, public_from_seed(seed))
    print(f"backend: {BACKEND}")
    print(f"private key: {private_path}")
    print(f"public key:  {public_path}")
    print("Publish only the .pub contents in ZTO_UPDATE_PUBLIC_KEY_BASE64.")
    return 0


def command_generate(args) -> int:
    package = Path(args.package).resolve()
    if not package.is_file():
        raise SystemExit(f"package does not exist: {package}")
    private = Path(args.private_key).resolve()
    if not private.is_file():
        raise SystemExit(f"private key does not exist: {private}")
    version_parts = args.version.split("-")[0].split(".")
    if len(version_parts) != 3 or not all(part.isdigit() for part in version_parts):
        raise SystemExit("--version must be MAJOR.MINOR.PATCH[-qualifier]")
    if args.min_updatable_version.split("-")[0].count(".") != 2:
        raise SystemExit("--min-updatable-version must be MAJOR.MINOR.PATCH[-qualifier]")
    payload = package.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    artifact = {
        "platform": args.platform,
        "arch": args.arch,
        "version": args.version,
        "url": args.url,
        "size": len(payload),
        "sha256": digest,
    }
    if args.remove_file:
        artifact["removeFiles"] = args.remove_file
    manifest = {
        "schemaVersion": 1,
        "channel": args.channel,
        "publishedAt": args.published_at,
        "minUpdatableVersion": args.min_updatable_version,
        "artifacts": [artifact],
    }
    # The signed bytes are exactly what is written and served. No client-side
    # canonicalization is permitted, so formatting changes are signature changes.
    manifest_bytes = (
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=False) + "\n"
    ).encode("utf-8")
    seed = read_seed(private)
    signature = sign_seed(seed, manifest_bytes)
    out = Path(args.out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "manifest.json").write_bytes(manifest_bytes)
    (out / "manifest.json.sig").write_text(
        base64.b64encode(signature).decode("ascii") + "\n", encoding="ascii"
    )
    print(f"backend: {BACKEND}")
    print(f"package: {package}")
    print(f"sha256:  {digest}")
    print(f"manifest: {out / 'manifest.json'}")
    print(f"signature: {out / 'manifest.json.sig'}")
    print(f"public key: {base64.b64encode(public_from_seed(seed)).decode('ascii')}")
    return 0


def command_verify(args) -> int:
    manifest = Path(args.manifest).resolve()
    signature_path = Path(args.signature).resolve()
    public = read_public(Path(args.public_key).resolve())
    payload = manifest.read_bytes()
    raw_signature = signature_path.read_text(encoding="ascii").strip()
    try:
        signature = base64.b64decode(raw_signature, validate=True)
    except Exception as exc:  # noqa: BLE001
        raise SystemExit(f"signature is not valid Base64: {exc}")
    if len(signature) != SIGNATURE_BYTES:
        raise SystemExit("signature must encode 64 bytes")
    if not verify_detached(public, payload, signature):
        raise SystemExit("signature verification FAILED")
    print(f"backend: {BACKEND}")
    print(f"signature OK for {len(payload)} manifest bytes")
    try:
        document = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"manifest is not valid JSON: {exc}")
    print(f"version: {document['artifacts'][0]['version']}")
    print(f"sha256:  {document['artifacts'][0]['sha256']}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    keygen = subparsers.add_parser("keygen", help="generate an Ed25519 signing key pair")
    keygen.add_argument("--out-dir", required=True)
    keygen.add_argument("--force", action="store_true")
    keygen.set_defaults(func=command_keygen)

    generate = subparsers.add_parser("generate", help="write manifest.json and manifest.json.sig")
    generate.add_argument("--package", required=True)
    generate.add_argument("--version", required=True)
    generate.add_argument("--platform", default="windows")
    generate.add_argument("--arch", default="x64")
    generate.add_argument("--url", required=True)
    generate.add_argument("--channel", default="stable")
    generate.add_argument("--min-updatable-version", required=True)
    generate.add_argument("--published-at", default=None)
    generate.add_argument("--private-key", required=True)
    generate.add_argument("--remove-file", action="append", default=[])
    generate.add_argument("--out-dir", required=True)
    generate.set_defaults(func=command_generate)

    verify = subparsers.add_parser("verify", help="verify a manifest signature")
    verify.add_argument("--manifest", required=True)
    verify.add_argument("--signature", required=True)
    verify.add_argument("--public-key", required=True)
    verify.set_defaults(func=command_verify)
    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "generate" and not args.published_at:
        from datetime import datetime, timezone

        args.published_at = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace(
            "+00:00", "Z"
        )
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
