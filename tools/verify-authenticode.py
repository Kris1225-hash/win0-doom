#!/usr/bin/env python3
"""Independent check of an extracted Authenticode PKCS#7 signature.

usage: verify-authenticode.py SIGNATURE.p7   (DER, from `osslsigncode extract-signature`)

build-release.sh normally checks the signature with `openssl smime -verify
-noverify`. OpenSSL 3.0.x (for example Ubuntu 24.04) rejects every valid
Authenticode signature there with "PKCS7_signatureVerify: digest failure",
because the signed content is SpcIndirectDataContent rather than plain data.
This script performs the same two checks that step exists for, chain ignored:

  1. the signer's messageDigest attribute equals the digest of the
     SpcIndirectDataContent value (which carries the PE image digest), and
  2. the signer's RSA PKCS#1 v1.5 signature over the DER signed attributes
     verifies with the certificate named by the signer's issuer and serial.

Exit status 0 means both hold. It needs python3 with the `cryptography` package.
"""

import hashlib
import sys
import warnings

from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding

# The WDK OEM test certificates carry negative serial numbers.
warnings.filterwarnings("ignore", message="Parsed a negative serial number")

OID_MESSAGE_DIGEST = bytes.fromhex("2a864886f70d010904")
DIGESTS = {
    bytes.fromhex("608648016503040201"): ("sha256", hashes.SHA256()),
    bytes.fromhex("608648016503040202"): ("sha384", hashes.SHA384()),
    bytes.fromhex("608648016503040203"): ("sha512", hashes.SHA512()),
    bytes.fromhex("2b0e03021a"): ("sha1", hashes.SHA1()),
}


class Tlv:
    def __init__(self, data, offset):
        if offset + 2 > len(data):
            raise ValueError("truncated DER")
        self.tag = data[offset]
        length = data[offset + 1]
        start = offset + 2
        if length & 0x80:
            count = length & 0x7F
            if count == 0 or count > 4:
                raise ValueError("unsupported DER length")
            length = int.from_bytes(data[start:start + count], "big")
            start += count
        if start + length > len(data):
            raise ValueError("DER element overruns its parent")
        self.offset, self.start, self.end = offset, start, start + length

    def children(self, data):
        items, cursor = [], self.start
        while cursor < self.end:
            item = Tlv(data, cursor)
            items.append(item)
            cursor = item.end
        return items

    def value(self, data):
        return data[self.start:self.end]


def fail(message):
    print(f"verify-authenticode: {message}", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) != 2:
        fail("usage: verify-authenticode.py SIGNATURE.p7")
    data = open(sys.argv[1], "rb").read()
    try:
        content_info = Tlv(data, 0).children(data)            # ContentInfo
        signed_data = content_info[1].children(data)[0].children(data)
        version, digest_algorithms, encap, *rest = signed_data
        encap_content = encap.children(data)[1].children(data)[0]  # [0] -> SpcIndirectDataContent
        certificates = next((item for item in rest if item.tag == 0xA0), None)
        signer_infos = rest[-1]
        if certificates is None or signer_infos.tag != 0x31:
            fail("no certificates or signer infos")
        signers = signer_infos.children(data)
        if len(signers) != 1:
            fail(f"expected one signer, found {len(signers)}")
        fields = signers[0].children(data)
        sid, digest_algorithm = fields[1], fields[2]
        signed_attrs = next((item for item in fields if item.tag == 0xA0), None)
        signature = [item for item in fields if item.tag == 0x04][-1]
        if signed_attrs is None:
            fail("signer has no signed attributes")
        serial_tlv = sid.children(data)[1]
        algorithm_oid = digest_algorithm.children(data)[0].value(data)
    except (IndexError, ValueError, StopIteration) as error:
        fail(f"malformed PKCS#7: {error}")

    if algorithm_oid not in DIGESTS:
        fail(f"unsupported digest algorithm {algorithm_oid.hex()}")
    digest_name, digest_hash = DIGESTS[algorithm_oid]

    message_digest = None
    for attribute in signed_attrs.children(data):
        oid, values = attribute.children(data)
        if oid.value(data) == OID_MESSAGE_DIGEST:
            message_digest = values.children(data)[0].value(data)
    if message_digest is None:
        fail("no messageDigest attribute")
    content_digest = hashlib.new(digest_name, encap_content.value(data)).digest()
    if message_digest != content_digest:
        fail("messageDigest does not match the SpcIndirectDataContent")

    serial = int.from_bytes(serial_tlv.value(data), "big", signed=True)
    signer = None
    for item in certificates.children(data):
        certificate = x509.load_der_x509_certificate(data[item.offset:item.end])
        if certificate.serial_number == serial:
            signer = certificate
    if signer is None:
        fail("the signer certificate is not embedded in the signature")

    # The signature covers the attributes DER-encoded as a SET, not as [0].
    attributes_der = b"\x31" + data[signed_attrs.offset + 1:signed_attrs.end]
    try:
        signer.public_key().verify(
            signature.value(data), attributes_der, padding.PKCS1v15(), digest_hash
        )
    except Exception as error:  # InvalidSignature or an unsupported key type
        fail(f"RSA signature over the signed attributes does not verify ({type(error).__name__})")
    print(f"verify-authenticode: ok ({digest_name}; signer {signer.subject.rfc4514_string()})")


if __name__ == "__main__":
    main()
