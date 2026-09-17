#!/usr/bin/env python3
"""Generate BearSSL trust anchors from a PEM bundle.

This is a build-time generator.  It accepts only X.509 CA certificates and
emits freestanding C data for RSA plus NIST P-256/P-384/P-521 ECDSA roots.
Unsupported public-key types are deliberately omitted; they cannot be safely
validated by the current BearSSL minimal verifier configuration.

SECURITY NOTE ON THE DEFAULT BUNDLE PATH
-----------------------------------------
The default --bundle used to be /etc/ssl/certs/ca-certificates.crt (the
BUILD MACHINE's live, ambient trust store). That is unsafe as a source for a
SHIPPED product's trust anchors: build machines - especially CI runners and
sandboxes - commonly have an organization-internal TLS-inspecting proxy CA
injected into their local trust store for that machine's own outbound
traffic. Generating from that store bakes trust in that proxy's CA into
every copy of the OS this build produces, which is a real regression found
in this exact tool: a prior run picked up four certificates named things
like "sandbox-egress-production TLS Inspection CA, O=Anthropic" from a build
sandbox and shipped them as trusted roots.

The default is now a pinned snapshot of Mozilla's public root program
(tools/mozilla_ca_bundle.pem, sourced from https://curl.se/ca/cacert.pem,
itself generated from Mozilla's own program - see the dated header inside
that file). It is checked into the tree specifically so this generator does
NOT depend on whatever an arbitrary build machine happens to trust locally.
Passing --bundle explicitly is still supported for testing, but doing so
against a live machine's ambient store should not be used for a release
build without inspecting the diff for anything that is not a recognised
public root CA.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import re
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa

CURVE_IDS = {
    ec.SECP256R1: "BR_EC_secp256r1",
    ec.SECP384R1: "BR_EC_secp384r1",
    ec.SECP521R1: "BR_EC_secp521r1",
}


def hex_rows(data: bytes, indent: str = "    ") -> str:
    rows = []
    for offset in range(0, len(data), 12):
        rows.append(indent + ", ".join(f"0x{v:02X}" for v in data[offset:offset + 12]) + ",")
    return "\n".join(rows)


def c_name(prefix: str, serial: int, cert: x509.Certificate) -> str:
    digest = hashlib.sha256(cert.public_bytes(serialization.Encoding.DER)).hexdigest()[:12].upper()
    return f"COS_CA_{prefix}_{serial:03d}_{digest}"


def main() -> int:
    parser = argparse.ArgumentParser()
    default_bundle = str(Path(__file__).resolve().parent / "mozilla_ca_bundle.pem")
    parser.add_argument("--bundle", default=default_bundle,
                        help="PEM bundle to generate from. Defaults to the pinned "
                             "Mozilla snapshot checked into tools/, NOT the build "
                             "machine's ambient store - see the module docstring.")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    # Comment lines in some distributions (e.g. curl.se/ca/cacert.pem) include
    # non-ASCII characters in human-readable CA names; only the base64 PEM
    # body needs to parse, so decode leniently rather than rejecting the
    # whole bundle over a comment.
    text = Path(args.bundle).read_text(encoding="utf-8", errors="replace")
    blocks = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", text, re.S)
    items = []
    skipped = []
    seen = set()

    for block in blocks:
        cert = x509.load_pem_x509_certificate(block.encode("ascii"))
        der = cert.public_bytes(serialization.Encoding.DER)
        digest = hashlib.sha256(der).digest()
        if digest in seen:
            continue
        seen.add(digest)
        # Keep only self-signed trust roots.  The distribution bundle normally
        # contains roots only, but this rejects accidental intermediate input.
        if cert.subject != cert.issuer:
            skipped.append((cert.subject.rfc4514_string(), "not self-signed"))
            continue
        name_der = cert.subject.public_bytes()
        key = cert.public_key()
        if isinstance(key, rsa.RSAPublicKey):
            numbers = key.public_numbers()
            n = numbers.n.to_bytes((numbers.n.bit_length() + 7) // 8, "big")
            e = numbers.e.to_bytes((numbers.e.bit_length() + 7) // 8, "big")
            if len(n) < 128:
                skipped.append((cert.subject.rfc4514_string(), "weak RSA key"))
                continue
            items.append(("RSA", cert, name_der, n, e, None))
        elif isinstance(key, ec.EllipticCurvePublicKey):
            curve_id = CURVE_IDS.get(type(key.curve))
            if curve_id is None:
                skipped.append((cert.subject.rfc4514_string(), f"unsupported EC curve {key.curve.name}"))
                continue
            q = key.public_bytes(serialization.Encoding.X962,
                                 serialization.PublicFormat.UncompressedPoint)
            items.append(("EC", cert, name_der, q, None, curve_id))
        else:
            skipped.append((cert.subject.rfc4514_string(), "unsupported public key"))

    import datetime
    lines = [
        "/* Auto-generated by tools/generate_bearssl_system_trust_store.py. */",
        "/* Source: a pinned Mozilla root-program snapshot, NOT the build",
        "   host's ambient CA store - see this file's module docstring. */",
        "/* Do not edit manually. */",
        f"/* Regenerate with: python3 tools/generate_bearssl_system_trust_store.py"
        f" --output {args.output} */",
        f"/* Generated: {datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d')} UTC"
        f" from {Path(args.bundle).name} */",
        "",
    ]
    names = []
    for idx, (kind, cert, dn, a, b, curve_id) in enumerate(items, 1):
        name = c_name(kind, idx, cert)
        names.append((name, kind, curve_id))
        lines.append(f"static const unsigned char {name}_DN[] = {{")
        lines.append(hex_rows(dn))
        lines.append("};\n")
        if kind == "RSA":
            lines.append(f"static const unsigned char {name}_N[] = {{")
            lines.append(hex_rows(a))
            lines.append("};")
            lines.append(f"static const unsigned char {name}_E[] = {{")
            lines.append(hex_rows(b))
            lines.append("};\n")
        else:
            lines.append(f"static const unsigned char {name}_Q[] = {{")
            lines.append(hex_rows(a))
            lines.append("};\n")

    lines.append("static const br_x509_trust_anchor COS_SYSTEM_TRUST_ANCHORS[] = {")
    for name, kind, curve_id in names:
        lines.append("    {")
        lines.append(f"        {{ (unsigned char *){name}_DN, sizeof {name}_DN }},")
        lines.append("        BR_X509_TA_CA,")
        if kind == "RSA":
            lines.append("        { BR_KEYTYPE_RSA, { .rsa = {")
            lines.append(f"            (unsigned char *){name}_N, sizeof {name}_N,")
            lines.append(f"            (unsigned char *){name}_E, sizeof {name}_E")
            lines.append("        } } }")
        else:
            lines.append("        { BR_KEYTYPE_EC, { .ec = {")
            lines.append(f"            {curve_id}, (unsigned char *){name}_Q, sizeof {name}_Q")
            lines.append("        } } }")
        lines.append("    },")
    lines.append("};")
    lines.append("#define COS_SYSTEM_TRUST_ANCHORS_COUNT \\")
    lines.append("    (sizeof(COS_SYSTEM_TRUST_ANCHORS) / sizeof(COS_SYSTEM_TRUST_ANCHORS[0]))")
    lines.append("")
    lines.append(f"/* Generated {len(items)} usable roots; skipped {len(skipped)} unsupported certificates. */")

    Path(args.output).write_text("\n".join(lines), encoding="ascii")
    print(f"generated {len(items)} usable trust anchors; skipped {len(skipped)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
