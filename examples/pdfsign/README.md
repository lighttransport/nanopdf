# pdfsign - PDF Digital Signature Tool

A CLI tool for signing, verifying, and inspecting PDF digital signatures.

## Features

- **Sign PDFs** with X.509 certificates (PEM format)
- **Verify signatures** and check document integrity
- **Display signature info** including MDP/certification details
- **Timestamp support** (RFC 3161)
- Support for **certification (DocMDP)** and **approval** signatures

## Building

First, build the nanopdf library:

```bash
cd ../..
mkdir -p build && cd build
cmake .. && make
```

Then build pdfsign:

```bash
cd examples/pdfsign
./build.sh
# or manually:
mkdir -p build && cd build
cmake .. && make
```

## Usage

### Display Signature Information

```bash
./build/pdfsign info document.pdf
```

Output:
```
Signature Information:
======================

Total signature fields: 2

Signature #1:
  Name: Signature1
  Signed: Yes
  Type: Certification (DocMDP)
  MDP Permissions: 2 (Form fill and sign only)
  Reason: Document certified
  Location: Tokyo, Japan
  Date: D:20240101120000+09'00'
  Filter: Adobe.PPKLite
  SubFilter: adbe.pkcs7.detached
  Timestamp: Yes (Embedded)
  Byte Range: [0, 1234, 5678, 9012]
  Bytes Covered: 10246
  Signature Size: 8192 bytes
  Integrity: INTACT (document not modified after signing)

Signature #2:
  Name: Signature2
  Signed: Yes
  Type: Approval
  Integrity: INTACT
```

### Verify Signatures

```bash
./build/pdfsign verify document.pdf
./build/pdfsign verify document.pdf -v  # verbose output
```

Output:
```
Verifying 2 signature(s)...

Signature #1 (Signature1): VALID (integrity check passed)
Signature #2 (Signature2): VALID (integrity check passed)

Summary: 2 passed, 0 failed
```

Verification status:
- `VALID` - Signature integrity verified
- `WARNING` - Document modified after signing
- `INVALID` - Signature verification failed
- `UNSIGNED` - Signature field exists but not signed

### Sign a PDF

```bash
# Approval signature
./build/pdfsign sign input.pdf signed.pdf --cert cert.pem --key key.pem

# With reason and location
./build/pdfsign sign input.pdf signed.pdf --cert cert.pem --key key.pem \
    --reason "Approved" --location "Tokyo, Japan"

# Certification signature (DocMDP)
./build/pdfsign sign input.pdf certified.pdf --cert cert.pem --key key.pem \
    --certify 2 --reason "Document certified"
```

### Add Document Timestamp

```bash
./build/pdfsign timestamp input.pdf timestamped.pdf --tsa http://timestamp.server/
```

## Command Reference

### Commands

| Command | Description |
|---------|-------------|
| `sign` | Sign a PDF with a certificate |
| `verify` | Verify signatures in a PDF |
| `info` | Display signature information |
| `timestamp` | Add a document timestamp |

### Sign Options

| Option | Description |
|--------|-------------|
| `--cert <file>` | Certificate file (PEM format) - required |
| `--key <file>` | Private key file (PEM format) |
| `--password <pass>` | Private key password |
| `--reason <text>` | Reason for signing |
| `--location <text>` | Signing location |
| `--contact <text>` | Contact information |
| `--certify <1-3>` | Create certification signature |

### Certification Levels (--certify)

| Level | Description |
|-------|-------------|
| 1 | No changes allowed after signing |
| 2 | Form filling and signing allowed |
| 3 | Form filling, signing, and annotations allowed |

### General Options

| Option | Description |
|--------|-------------|
| `-v, --verbose` | Verbose output |
| `--help` | Show help message |

## Certificate Format

pdfsign supports X.509 certificates in PEM format:

```
-----BEGIN CERTIFICATE-----
MIICpDCCAYwCCQDU+pQ4P1cBYDANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls
...
-----END CERTIFICATE-----
```

Private keys should also be in PEM format:

```
-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC7...
-----END PRIVATE KEY-----
```

## Signature Types

### Approval Signature
Standard signature that does not restrict future modifications.

### Certification Signature (DocMDP)
First signature that certifies the document and specifies what changes
are allowed afterward:
- **P=1**: No changes allowed
- **P=2**: Form filling and signing only
- **P=3**: Form filling, signing, and annotations

## Verification Process

1. **Byte Range Check**: Verify signature covers expected document portions
2. **Integrity Check**: Detect modifications after signing
3. **Hash Verification**: Compute and compare document hash
4. **Timestamp Validation**: Verify embedded timestamps (if present)

## Limitations

- Full cryptographic verification requires OpenSSL integration
- PKCS#12 (.p12/.pfx) files not yet supported
- TSA timestamping requires network access
- Some advanced signature features may not be supported

## Integration with OpenSSL

For production use with full cryptographic operations:

```bash
# Generate test certificate
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes

# Use with pdfsign
./build/pdfsign sign input.pdf signed.pdf --cert cert.pem --key key.pem
```
