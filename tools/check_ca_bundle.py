"""Offline release gate for known unsafe TLS trust anchors; never edits a store."""

import argparse
import hashlib
from pathlib import Path
import re
import ssl


# Published certificate SHA-256 fingerprints, not names supplied by a certificate.
# https://emudhra.we-have-a-problem.com/roots.json (October 2026 incident)
# These are TLS trust paths to the compromised intermediates, not a blanket ban
# on eMudhra/Sectigo. This gate supplements distro updates, not revocation checks.
BLOCKED_ANCHORS = {
    "86a1ecba089c4a8d3bbe2734c612ba341d813e043cf9e8a862cd5c57a36bbe6b": "emSign ECC Root CA - G3",
    "d7a7a0fb5d7e2731d771e9484ebcdef71d5f0c3e0a2948782bc83ee0ea699ef4": "AAA Certificate Services",
    "7dd78d5f4f13459a83dff9abbba62edbaf6f2d102bf257fd712f4d9f2746ed8d": "emSign Root TLS CA - G3",
}
POLICY = "emudhra-2026-10-20-v1"


def check_ca_bundle(path):
    path = Path(path)
    content = path.read_text(encoding="utf-8")
    certificates = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", content, re.S)
    if not certificates:
        raise RuntimeError("CA bundle contains no certificates")
    try:
        # Parse with OpenSSL too: base64 decoding alone does not validate X.509.
        ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT).load_verify_locations(cafile=str(path))
        fingerprints = {hashlib.sha256(ssl.PEM_cert_to_DER_cert(cert)).hexdigest() for cert in certificates}
    except (ssl.SSLError, ValueError) as error:
        raise RuntimeError("invalid CA certificate bundle") from error
    blocked = sorted(BLOCKED_ANCHORS[digest] for digest in fingerprints if digest in BLOCKED_ANCHORS)
    if blocked:
        raise RuntimeError("unsafe CA bundle; update ca-certificates before packaging: " + ", ".join(blocked))
    print(f"CA bundle passed {POLICY}: {len(certificates)} certificates", flush=True)
    return {"policy": POLICY, "certificates": len(certificates)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    args = parser.parse_args()
    try:
        check_ca_bundle(args.bundle)
    except (OSError, UnicodeError, RuntimeError) as error:
        parser.exit(1, f"CA bundle check failed: {error}\n")
