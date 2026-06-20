/* SPDX-License-Identifier: Apache-2.0
 * RFC 3161 timestamping: build a TimeStampReq and parse a TimeStampResp. */
#ifndef NCRYPTO_NC_RFC3161_H_
#define NCRYPTO_NC_RFC3161_H_

#include <stddef.h>
#include <stdint.h>

#include "ncrypto/nc_asn1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Dotted OID for a hash name ("sha256","sha384","sha512","sha1"); returns the
 * string (static) or NULL if unknown. */
const char* nc_rfc3161_hash_oid(const char* name);

/* Build a DER TimeStampReq for @imprint (the message digest) under hash OID
 * @hash_oid_dotted, with @nonce and certReq=@cert_req. Writes to @out.
 * Returns 0 on success. */
int nc_rfc3161_build_request(nc_buf* out, const uint8_t* imprint,
                             size_t imprint_len, const char* hash_oid_dotted,
                             uint64_t nonce, int cert_req);

/* Parse a DER TimeStampResp. Sets *status to the PKIStatus integer (0 granted,
 * 1 grantedWithMods, else rejected). On a granted response, appends the
 * timeStampToken (a ContentInfo DER) to @token_out. Returns 0 on success. */
int nc_rfc3161_parse_response(const uint8_t* tsr, size_t tsr_len, int* status,
                              nc_buf* token_out);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_RFC3161_H_ */
