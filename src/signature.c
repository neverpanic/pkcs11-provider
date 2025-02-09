/* Copyright (C) 2022 Simo Sorce <simo@redhat.com>
   SPDX-License-Identifier: Apache-2.0 */

#include "provider.h"
#include <string.h>
#include "openssl/evp.h"
#include "openssl/rsa.h"
#include "openssl/ec.h"
#include "openssl/sha.h"

struct p11prov_sig_ctx {
    P11PROV_CTX *provctx;
    char *properties;

    P11PROV_OBJ *key;

    CK_MECHANISM_TYPE mechtype;
    CK_MECHANISM_TYPE digest;

    CK_FLAGS operation;
    P11PROV_SESSION *session;

    CK_RSA_PKCS_PSS_PARAMS pss_params;
};
typedef struct p11prov_sig_ctx P11PROV_SIG_CTX;

DISPATCH_SIG_FN(freectx);
DISPATCH_SIG_FN(dupctx);

static P11PROV_SIG_CTX *p11prov_sig_newctx(P11PROV_CTX *ctx,
                                           CK_MECHANISM_TYPE type,
                                           const char *properties)
{
    P11PROV_SIG_CTX *sigctx;

    sigctx = OPENSSL_zalloc(sizeof(P11PROV_SIG_CTX));
    if (sigctx == NULL) {
        return NULL;
    }

    sigctx->provctx = ctx;

    if (properties) {
        sigctx->properties = OPENSSL_strdup(properties);
        if (sigctx->properties == NULL) {
            OPENSSL_free(sigctx);
            return NULL;
        }
    }

    sigctx->mechtype = type;

    return sigctx;
}

static void *p11prov_sig_dupctx(void *ctx)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    P11PROV_SIG_CTX *newctx;
    CK_SLOT_ID slotid = CK_UNAVAILABLE_INFORMATION;
    CK_OBJECT_HANDLE handle = CK_INVALID_HANDLE;
    CK_BYTE_PTR state = NULL;
    CK_ULONG state_len;
    bool reqlogin = false;
    CK_RV ret;

    if (sigctx == NULL) {
        return NULL;
    }

    switch (sigctx->operation) {
    case CKF_SIGN:
        reqlogin = true;
        /* fallthrough */
    case CKF_VERIFY:
        slotid = p11prov_obj_get_slotid(sigctx->key);
        handle = p11prov_obj_get_handle(sigctx->key);
        break;
    default:
        return NULL;
    }

    newctx = p11prov_sig_newctx(sigctx->provctx, sigctx->mechtype,
                                sigctx->properties);
    if (newctx == NULL) {
        return NULL;
    }

    newctx->key = p11prov_obj_ref(sigctx->key);
    newctx->mechtype = sigctx->mechtype;
    newctx->digest = sigctx->digest;
    newctx->pss_params = sigctx->pss_params;
    newctx->operation = sigctx->operation;

    if (sigctx->session == NULL) {
        goto done;
    }

    /* This is not really funny. OpenSSL by default assumes contexts with
     * operations in flight can be easily duplicated, with all the
     * cryptographic status and then both contexts can keep going
     * independently. We'll try here, but on failure we just 'move' the
     * session to the new context (because that's what OpenSSL seem to
     * prefer to use after duplication) and hope for the best. */

    newctx->session = sigctx->session;
    sigctx->session = NULL;

    if (slotid != CK_UNAVAILABLE_INFORMATION && handle != CK_INVALID_HANDLE) {
        CK_SESSION_HANDLE newsess = p11prov_session_handle(newctx->session);
        CK_SESSION_HANDLE sess = CK_INVALID_HANDLE;

        ret = p11prov_GetOperationState(sigctx->provctx, newsess, NULL_PTR,
                                        &state_len);
        if (ret != CKR_OK) {
            goto done;
        }
        state = OPENSSL_malloc(state_len);
        if (state == NULL) {
            goto done;
        }

        ret = p11prov_GetOperationState(sigctx->provctx, newsess, state,
                                        &state_len);
        if (ret != CKR_OK) {
            goto done;
        }

        ret = p11prov_get_session(sigctx->provctx, &slotid, NULL, NULL,
                                  sigctx->mechtype, NULL, NULL, reqlogin, false,
                                  &sigctx->session);
        if (ret != CKR_OK) {
            P11PROV_raise(sigctx->provctx, ret,
                          "Failed to open session on slot %lu", slotid);
            goto done;
        }
        sess = p11prov_session_handle(sigctx->session);

        ret = p11prov_SetOperationState(sigctx->provctx, sess, state, state_len,
                                        handle, handle);
        if (ret != CKR_OK) {
            p11prov_return_session(sigctx->session);
            sigctx->session = NULL;
        }
    }

done:
    OPENSSL_free(state);
    return newctx;
}

static void p11prov_sig_freectx(void *ctx)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;

    if (sigctx == NULL) {
        return;
    }

    p11prov_return_session(sigctx->session);
    p11prov_obj_free(sigctx->key);
    OPENSSL_free(sigctx->properties);
    OPENSSL_clear_free(sigctx, sizeof(P11PROV_SIG_CTX));
}

#define DER_SEQUENCE 0x30
#define DER_OBJECT 0x06
#define DER_NULL 0x05
#define DER_OCTET_STRING 0x04

/* iso(1) member-body(2) us(840) rsadsi(113549) pkcs(1) pkcs-1(1) */
#define DER_RSADSI_PKCS1 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01
#define DER_RSADSI_PKCS1_LEN 0x08

/* iso(1) member-body(2) us(840) ansi-x962(10045) signatures(4) */
#define DER_ANSIX962_SIG 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04
#define DER_ANSIX962_SIG_LEN 0x06

/* ... ansi-x962(10045) signatures(4) ecdsa-with-SHA2(3) */
#define DER_ANSIX962_SHA2_SIG DER_ANSIX962_SIG, 0x03
#define DER_ANSIX962_SHA2_SIG_LEN (DER_ANSIX962_SIG_LEN + 1)

/* joint-iso-itu-t(2) country(16) us(840) organization(1) gov(101) csor(3)
 * nistAlgorithms(4) */
#define DER_NIST_ALGS 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04
#define DER_NIST_ALGS_LEN 0x07

/* ... csor(3) nistAlgorithms(4) hashalgs(2) */
#define DER_NIST_HASHALGS DER_NIST_ALGS, 0x02
#define DER_NIST_HASHALGS_LEN (DER_NIST_ALGS_LEN + 1)

/* ... csor(3) nistAlgorithms(4) sigAlgs(3) */
#define DER_NIST_SIGALGS DER_NIST_ALGS, 0x03
#define DER_NIST_SIGALGS_LEN (DER_NIST_ALGS_LEN + 1)

/* clang-format off */
#define DEFINE_DER_DIGESTINFO(name, alg_id, digest_size) \
    static const unsigned char der_digestinfo_##name[] = { \
        DER_SEQUENCE, DER_NIST_HASHALGS_LEN+9+digest_size, \
          DER_SEQUENCE, DER_NIST_HASHALGS_LEN+5, \
            DER_OBJECT, DER_NIST_HASHALGS_LEN+1, DER_NIST_HASHALGS, alg_id, \
            DER_NULL, 0, \
          DER_OCTET_STRING, digest_size \
    };

#define DEFINE_DER_SEQ_SHA(bits, rsa_algid, ecdsa_algid, digestinfo_algid) \
    static const unsigned char der_rsa_sha##bits[] = { \
        DER_SEQUENCE, DER_RSADSI_PKCS1_LEN+5, \
            DER_OBJECT, DER_RSADSI_PKCS1_LEN+1, DER_RSADSI_PKCS1, rsa_algid, \
            DER_NULL, 0, \
    }; \
    static const unsigned char der_ecdsa_sha##bits[] = { \
        DER_SEQUENCE, DER_ANSIX962_SHA2_SIG_LEN+3, \
            DER_OBJECT, DER_ANSIX962_SHA2_SIG_LEN+1, DER_ANSIX962_SHA2_SIG, ecdsa_algid, \
    }; \
    DEFINE_DER_DIGESTINFO(sha##bits, digestinfo_algid, bits/8)

#define DEFINE_DER_SEQ_SHA3(bits, rsa_algid, ecdsa_algid, digestinfo_algid) \
    static const unsigned char der_rsa_sha3_##bits[] = { \
        DER_SEQUENCE, DER_NIST_SIGALGS_LEN+5, \
            DER_OBJECT, DER_NIST_SIGALGS_LEN+1, DER_NIST_SIGALGS, rsa_algid, \
            DER_NULL, 0 \
    }; \
    static const unsigned char der_ecdsa_sha3_##bits[] = { \
        DER_SEQUENCE, DER_NIST_SIGALGS_LEN+3, \
            DER_OBJECT, DER_NIST_SIGALGS_LEN+1, DER_NIST_SIGALGS, ecdsa_algid \
    }; \
    DEFINE_DER_DIGESTINFO(sha3_##bits, digestinfo_algid, bits/8)

static const unsigned char der_rsa_sha1[] = {
    DER_SEQUENCE, DER_RSADSI_PKCS1_LEN+5,
        DER_OBJECT, DER_RSADSI_PKCS1_LEN+1, DER_RSADSI_PKCS1, 0x05,
        DER_NULL, 0
};
static const unsigned char der_ecdsa_sha1[] = {
    DER_SEQUENCE, DER_ANSIX962_SIG_LEN+3,
        DER_OBJECT, DER_ANSIX962_SIG_LEN+1, DER_ANSIX962_SIG, 0x01
};
/* iso(1) org(3) oiw(14) secsig(3) algorithms(2) hashAlgorithmIdentifier(26) */
static const unsigned char der_digestinfo_sha1[] = {
    DER_SEQUENCE, 0x0d + SHA_DIGEST_LENGTH,
        DER_SEQUENCE, 0x09,
        DER_OBJECT, 0x05, 1 * 40 + 3, 14, 3, 2, 26,
        DER_NULL, 0x00,
    DER_OCTET_STRING, SHA_DIGEST_LENGTH
};
/* clang-format on */

DEFINE_DER_SEQ_SHA(512, 0x0D, 0x04, 0x03);
DEFINE_DER_SEQ_SHA(384, 0x0C, 0x03, 0x02);
DEFINE_DER_SEQ_SHA(256, 0x0B, 0x02, 0x01);
DEFINE_DER_SEQ_SHA(224, 0x0E, 0x01, 0x04);

DEFINE_DER_SEQ_SHA3(512, 0x10, 0x0C, 0x0A);
DEFINE_DER_SEQ_SHA3(384, 0x0F, 0x0B, 0x09);
DEFINE_DER_SEQ_SHA3(256, 0x0E, 0x0A, 0x08);
DEFINE_DER_SEQ_SHA3(224, 0x0D, 0x09, 0x07);

#define DM_ELEM_SHA(bits) \
    { \
        .digest = CKM_SHA##bits, .pkcs_mech = CKM_SHA##bits##_RSA_PKCS, \
        .pkcs_pss = CKM_SHA##bits##_RSA_PKCS_PSS, \
        .ecdsa_mech = CKM_ECDSA_SHA##bits, .mgf = CKG_MGF1_SHA##bits, \
        .der_rsa_algorithm_id = der_rsa_sha##bits, \
        .der_rsa_algorithm_id_len = sizeof(der_rsa_sha##bits), \
        .der_ecdsa_algorithm_id = der_ecdsa_sha##bits, \
        .der_ecdsa_algorithm_id_len = sizeof(der_ecdsa_sha##bits), \
        .der_digestinfo = der_digestinfo_sha##bits, \
        .der_digestinfo_len = sizeof(der_digestinfo_sha##bits), \
    }
#define DM_ELEM_SHA3(bits) \
    { \
        .digest = CKM_SHA3_##bits, .pkcs_mech = CKM_SHA3_##bits##_RSA_PKCS, \
        .pkcs_pss = CKM_SHA3_##bits##_RSA_PKCS_PSS, \
        .ecdsa_mech = CKM_ECDSA_SHA3_##bits, .mgf = CKG_MGF1_SHA3_##bits, \
        .der_rsa_algorithm_id = der_rsa_sha3_##bits, \
        .der_rsa_algorithm_id_len = sizeof(der_rsa_sha3_##bits), \
        .der_ecdsa_algorithm_id = der_ecdsa_sha3_##bits, \
        .der_ecdsa_algorithm_id_len = sizeof(der_ecdsa_sha3_##bits), \
        .der_digestinfo = der_digestinfo_sha3_##bits, \
        .der_digestinfo_len = sizeof(der_digestinfo_sha3_##bits), \
    }

/* only the ones we can support */
struct p11prov_mech {
    CK_MECHANISM_TYPE digest;
    CK_MECHANISM_TYPE pkcs_mech;
    CK_MECHANISM_TYPE pkcs_pss;
    CK_MECHANISM_TYPE ecdsa_mech;
    CK_RSA_PKCS_MGF_TYPE mgf;
    const unsigned char *der_rsa_algorithm_id;
    int der_rsa_algorithm_id_len;
    const unsigned char *der_ecdsa_algorithm_id;
    int der_ecdsa_algorithm_id_len;
    const unsigned char *der_digestinfo;
    int der_digestinfo_len;
};
typedef struct p11prov_mech P11PROV_MECH;

static const P11PROV_MECH mech_map[] = {
    DM_ELEM_SHA3(256),
    DM_ELEM_SHA3(512),
    DM_ELEM_SHA3(384),
    DM_ELEM_SHA3(224),
    DM_ELEM_SHA(256),
    DM_ELEM_SHA(512),
    DM_ELEM_SHA(384),
    DM_ELEM_SHA(224),
    { CKM_SHA_1, CKM_SHA1_RSA_PKCS, CKM_SHA1_RSA_PKCS_PSS, CKM_ECDSA_SHA1,
      CKG_MGF1_SHA1, der_rsa_sha1, sizeof(der_rsa_sha1), der_ecdsa_sha1,
      sizeof(der_ecdsa_sha1), der_digestinfo_sha1,
      sizeof(der_digestinfo_sha1) },
    { CK_UNAVAILABLE_INFORMATION, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static CK_RV p11prov_mech_by_mechanism(CK_MECHANISM_TYPE mechanism,
                                       const P11PROV_MECH **mech)
{
    for (int i = 0; mech_map[i].digest != CK_UNAVAILABLE_INFORMATION; i++) {
        if (mech_map[i].digest == mechanism) {
            *mech = &mech_map[i];
            return CKR_OK;
        }
    }
    return CKR_MECHANISM_INVALID;
}

static CK_RV p11prov_mech_by_mgf(CK_RSA_PKCS_MGF_TYPE mgf,
                                 const P11PROV_MECH **mech)
{
    for (int i = 0; mech_map[i].digest != CK_UNAVAILABLE_INFORMATION; i++) {
        if (mech_map[i].mgf == mgf) {
            *mech = &mech_map[i];
            return CKR_OK;
        }
    }
    return CKR_MECHANISM_INVALID;
}

static const char *p11prov_sig_mgf_name(CK_RSA_PKCS_MGF_TYPE mgf)
{
    const P11PROV_MECH *mech = NULL;
    const char *digest_name;
    CK_RV rv;

    rv = p11prov_mech_by_mgf(mgf, &mech);
    if (rv != CKR_OK) {
        return NULL;
    }

    rv = p11prov_digest_get_name(mech->digest, &digest_name);
    if (rv != CKR_OK) {
        return NULL;
    }

    return digest_name;
}

static CK_RSA_PKCS_MGF_TYPE p11prov_sig_map_mgf(const char *digest_name)
{
    CK_MECHANISM_TYPE digest;
    const P11PROV_MECH *mech = NULL;
    CK_RV rv;

    rv = p11prov_digest_get_by_name(digest_name, &digest);
    if (rv != CKR_OK) {
        return CK_UNAVAILABLE_INFORMATION;
    }

    rv = p11prov_mech_by_mechanism(digest, &mech);
    if (rv != CKR_OK) {
        return CK_UNAVAILABLE_INFORMATION;
    }

    return mech->mgf;
}

static CK_RV p11prov_sig_pss_restrictions(P11PROV_SIG_CTX *sigctx,
                                          CK_MECHANISM *mechanism)
{
    CK_ATTRIBUTE *allowed_mechs =
        p11prov_obj_get_attr(sigctx->key, CKA_ALLOWED_MECHANISMS);

    if (allowed_mechs) {
        CK_ATTRIBUTE_TYPE *mechs = (CK_ATTRIBUTE_TYPE *)allowed_mechs->pValue;
        int num_mechs = allowed_mechs->ulValueLen;
        bool allowed = false;

        for (int i = 0; i < num_mechs; i++) {
            if (mechs[i] == mechanism->mechanism) {
                allowed = true;
                break;
            }
        }

        if (allowed) {
            return CKR_OK;
        }

        P11PROV_raise(sigctx->provctx, CKR_ACTION_PROHIBITED,
                      "mechanism not allowed with this key");
        return CKR_ACTION_PROHIBITED;
    }

    /* there are no restrictions on this key */
    return CKR_OK;
}

static int p11prov_sig_set_mechanism(void *ctx, bool digest_sign,
                                     CK_MECHANISM *mechanism)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    const P11PROV_MECH *mech;
    int result = CKR_DATA_INVALID;

    mechanism->mechanism = sigctx->mechtype;
    mechanism->pParameter = NULL;
    mechanism->ulParameterLen = 0;

    if (sigctx->mechtype == CKM_RSA_PKCS_PSS) {
        mechanism->pParameter = &sigctx->pss_params;
        mechanism->ulParameterLen = sizeof(sigctx->pss_params);
        if (sigctx->digest) {
            sigctx->pss_params.hashAlg = sigctx->digest;
        }
    }

    if (!digest_sign) {
        return CKR_OK;
    }

    switch (sigctx->mechtype) {
    case CKM_RSA_PKCS:
        result = p11prov_mech_by_mechanism(sigctx->digest, &mech);
        if (result == CKR_OK) {
            mechanism->mechanism = mech->pkcs_mech;
        }
        break;
    case CKM_RSA_X_509:
        break;
    case CKM_RSA_PKCS_PSS:
        mechanism->pParameter = &sigctx->pss_params;
        mechanism->ulParameterLen = sizeof(sigctx->pss_params);

        result = p11prov_mech_by_mechanism(sigctx->digest, &mech);
        if (result == CKR_OK) {
            mechanism->mechanism = mech->pkcs_pss;
            result = p11prov_sig_pss_restrictions(ctx, mechanism);
        }
        break;
    case CKM_ECDSA:
        result = p11prov_mech_by_mechanism(sigctx->digest, &mech);
        if (result == CKR_OK) {
            mechanism->mechanism = mech->ecdsa_mech;
        }
        break;
    }

    if (result == CKR_OK) {
        P11PROV_debug_mechanism(sigctx->provctx,
                                p11prov_obj_get_slotid(sigctx->key),
                                mechanism->mechanism);
    }
    return result;
}

static int p11prov_sig_get_sig_size(void *ctx, size_t *siglen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    CK_KEY_TYPE type = p11prov_obj_get_key_type(sigctx->key);
    CK_ULONG size = p11prov_obj_get_key_size(sigctx->key);

    if (type == CK_UNAVAILABLE_INFORMATION) {
        return RET_OSSL_ERR;
    }
    if (size == CK_UNAVAILABLE_INFORMATION) {
        return RET_OSSL_ERR;
    }

    switch (type) {
    case CKK_RSA:
        *siglen = size;
        break;
    case CKK_EC:
        /* add room for ECDSA Signature DER overhead */
        *siglen = 3 + (size + 4) * 2;
        break;
    default:
        return RET_OSSL_ERR;
    }
    return RET_OSSL_OK;
}

static int p11prov_rsasig_set_pss_saltlen_from_digest(void *ctx)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    size_t digest_size;
    CK_RV rv;

    if (sigctx->digest == 0) {
        ERR_raise_data(ERR_LIB_PROV, PROV_R_NOT_SUPPORTED,
                       "Can only be set if Digest was set first.");
        return RET_OSSL_ERR;
    }

    rv = p11prov_digest_get_digest_size(sigctx->digest, &digest_size);
    if (rv != CKR_OK) {
        P11PROV_raise(sigctx->provctx, rv, "Unavailable digest");
        return RET_OSSL_ERR;
    }

    sigctx->pss_params.sLen = digest_size;
    return RET_OSSL_OK;
}

static int p11prov_sig_op_init(void *ctx, void *provkey, CK_FLAGS operation,
                               const char *digest, const OSSL_PARAM params[])
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    P11PROV_OBJ *key = (P11PROV_OBJ *)provkey;
    CK_OBJECT_CLASS class;
    CK_RV ret;

    ret = p11prov_ctx_status(sigctx->provctx);
    if (ret != CKR_OK) {
        return RET_OSSL_ERR;
    }

    sigctx->key = p11prov_obj_ref(key);
    if (sigctx->key == NULL) {
        return RET_OSSL_ERR;
    }
    class = p11prov_obj_get_class(sigctx->key);
    switch (operation) {
    case CKF_SIGN:
        if (class != CKO_PRIVATE_KEY) {
            return RET_OSSL_ERR;
        }
        break;
    case CKF_VERIFY:
        if (class != CKO_PUBLIC_KEY) {
            return RET_OSSL_ERR;
        }
        break;
    default:
        return RET_OSSL_ERR;
    }
    sigctx->operation = operation;

    if (digest) {
        CK_RV rv;

        rv = p11prov_digest_get_by_name(digest, &sigctx->digest);
        if (rv != CKR_OK) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_DIGEST);
            return RET_OSSL_ERR;
        }
    }

    return RET_OSSL_OK;
}

static int p11prov_sig_operate_init(P11PROV_SIG_CTX *sigctx, bool digest_op,
                                    P11PROV_SESSION **_session)
{
    CK_MECHANISM mechanism;
    P11PROV_SESSION *session;
    CK_SESSION_HANDLE sess;
    CK_OBJECT_HANDLE handle;
    CK_SLOT_ID slotid;
    bool reqlogin = false;
    CK_RV ret;

    ret = p11prov_ctx_status(sigctx->provctx);
    if (ret != CKR_OK) {
        return ret;
    }

    handle = p11prov_obj_get_handle(sigctx->key);
    if (handle == CK_INVALID_HANDLE) {
        P11PROV_raise(sigctx->provctx, CKR_KEY_HANDLE_INVALID,
                      "Provided key has invalid handle");
        return CKR_KEY_HANDLE_INVALID;
    }
    slotid = p11prov_obj_get_slotid(sigctx->key);
    if (slotid == CK_UNAVAILABLE_INFORMATION) {
        P11PROV_raise(sigctx->provctx, CKR_SLOT_ID_INVALID,
                      "Provided key has invalid slot");
        return CKR_SLOT_ID_INVALID;
    }

    ret = p11prov_sig_set_mechanism(sigctx, digest_op, &mechanism);
    if (ret != CKR_OK) {
        return ret;
    }

    if (sigctx->operation == CKF_SIGN) {
        reqlogin = true;
    }

    ret = p11prov_get_session(sigctx->provctx, &slotid, NULL, NULL,
                              mechanism.mechanism, NULL, NULL, reqlogin, false,
                              &session);
    if (ret != CKR_OK) {
        P11PROV_raise(sigctx->provctx, ret,
                      "Failed to open session on slot %lu", slotid);
        return ret;
    }
    sess = p11prov_session_handle(session);

    if (sigctx->operation == CKF_SIGN) {
        ret = p11prov_SignInit(sigctx->provctx, sess, &mechanism, handle);
    } else {
        ret = p11prov_VerifyInit(sigctx->provctx, sess, &mechanism, handle);
    }
    if (ret != CKR_OK) {
        int result = ret;
        if (ret == CKR_MECHANISM_INVALID
            || ret == CKR_MECHANISM_PARAM_INVALID) {
            ERR_raise(ERR_LIB_PROV, PROV_R_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE);
        }
        p11prov_return_session(session);
        return result;
    }

    *_session = session;
    return CKR_OK;
}

static int p11prov_sig_operate(P11PROV_SIG_CTX *sigctx, unsigned char *sig,
                               size_t *siglen, size_t sigsize,
                               unsigned char *tbs, size_t tbslen)
{
    P11PROV_SESSION *session;
    CK_SESSION_HANDLE sess;
    CK_ULONG sig_size = sigsize;
    int result = RET_OSSL_ERR;
    CK_RV ret;
    /* The 64 is to accommodate largest possible der_digestinfo prefix encoding */
    unsigned char data[EVP_MAX_MD_SIZE + 64];

    if (sig == NULL) {
        if (sigctx->operation == CKF_VERIFY) {
            return RET_OSSL_ERR;
        }
        return p11prov_sig_get_sig_size(sigctx, siglen);
    }

    if (sigctx->operation == CKF_SIGN && sigctx->mechtype == CKM_RSA_X_509) {
        /* some tokens allow raw signatures on any data size.
         * Enforce data size is the same as modulus as that is
         * what OpenSSL expects and does internally in rsa_sign
         * when there is no padding. */
        if (tbslen < sigsize) {
            ERR_raise(ERR_LIB_RSA, RSA_R_DATA_TOO_SMALL_FOR_KEY_SIZE);
            return RET_OSSL_ERR;
        }
    }

    if (sigctx->mechtype == CKM_RSA_PKCS && sigctx->digest != 0) {
        const P11PROV_MECH *mech = NULL;
        size_t digest_size = 0;

        ret = p11prov_mech_by_mechanism(sigctx->digest, &mech);
        if (ret != CKR_OK) {
            ERR_raise(ERR_LIB_RSA, PROV_R_INVALID_DIGEST);
            return RET_OSSL_ERR;
        }
        ret = p11prov_digest_get_digest_size(sigctx->digest, &digest_size);
        if (ret != CKR_OK) {
            ERR_raise(ERR_LIB_RSA, PROV_R_INVALID_DIGEST);
            return RET_OSSL_ERR;
        }
        if (tbslen != digest_size
            || tbslen + mech->der_digestinfo_len >= sizeof(data)) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_INPUT_LENGTH);
            return RET_OSSL_ERR;
        }
        memcpy(data, mech->der_digestinfo, mech->der_digestinfo_len);
        memcpy(data + mech->der_digestinfo_len, tbs, tbslen);
        tbs = data;
        tbslen += mech->der_digestinfo_len;
    }

    ret = p11prov_sig_operate_init(sigctx, false, &session);
    if (ret != CKR_OK) {
        return RET_OSSL_ERR;
    }
    sess = p11prov_session_handle(session);

    if (sigctx->operation == CKF_SIGN) {
        ret = p11prov_Sign(sigctx->provctx, sess, tbs, tbslen, sig, &sig_size);
    } else {
        ret = p11prov_Verify(sigctx->provctx, sess, tbs, tbslen, sig, sigsize);
    }
    if (ret != CKR_OK) {
        goto endsess;
    }

    if (siglen) {
        *siglen = sig_size;
    }
    result = RET_OSSL_OK;

endsess:
    p11prov_return_session(session);
    if (tbs == data) {
        OPENSSL_cleanse(data, sizeof(data));
    }
    return result;
}

static int p11prov_sig_digest_update(P11PROV_SIG_CTX *sigctx,
                                     unsigned char *data, size_t datalen)
{
    CK_SESSION_HANDLE sess;
    CK_RV ret;

    if (sigctx->session == CK_INVALID_HANDLE) {
        ret = p11prov_sig_operate_init(sigctx, true, &sigctx->session);
        if (ret != CKR_OK) {
            return RET_OSSL_ERR;
        }
    }
    sess = p11prov_session_handle(sigctx->session);

    /* we have an initialized session */
    if (sigctx->operation == CKF_SIGN) {
        ret = p11prov_SignUpdate(sigctx->provctx, sess, data, datalen);
    } else {
        ret = p11prov_VerifyUpdate(sigctx->provctx, sess, data, datalen);
    }
    if (ret != CKR_OK) {
        p11prov_return_session(sigctx->session);
        sigctx->session = NULL;
        return RET_OSSL_ERR;
    }

    return RET_OSSL_OK;
}

static int p11prov_sig_digest_final(P11PROV_SIG_CTX *sigctx, unsigned char *sig,
                                    size_t *siglen, size_t sigsize)
{
    CK_SESSION_HANDLE sess;
    CK_ULONG sig_size = sigsize;
    int result = RET_OSSL_ERR;
    CK_RV ret;

    if (sig == NULL) {
        if (sigctx->operation == CKF_VERIFY) {
            return RET_OSSL_ERR;
        }
        return p11prov_sig_get_sig_size(sigctx, siglen);
    }

    if (!sigctx->session) {
        return RET_OSSL_ERR;
    }
    sess = p11prov_session_handle(sigctx->session);

    if (sigctx->operation == CKF_SIGN) {
        ret = p11prov_SignFinal(sigctx->provctx, sess, sig, &sig_size);
    } else {
        ret = p11prov_VerifyFinal(sigctx->provctx, sess, sig, sigsize);
    }

    if (ret == CKR_OK) {
        if (siglen) {
            *siglen = sig_size;
        }
        result = RET_OSSL_OK;
    }

    p11prov_return_session(sigctx->session);
    sigctx->session = NULL;
    return result;
}

DISPATCH_RSASIG_FN(newctx);
DISPATCH_RSASIG_FN(sign_init);
DISPATCH_RSASIG_FN(sign);
DISPATCH_RSASIG_FN(verify_init);
DISPATCH_RSASIG_FN(verify);
DISPATCH_RSASIG_FN(digest_sign_init);
DISPATCH_RSASIG_FN(digest_sign_update);
DISPATCH_RSASIG_FN(digest_sign_final);
DISPATCH_RSASIG_FN(digest_verify_init);
DISPATCH_RSASIG_FN(digest_verify_update);
DISPATCH_RSASIG_FN(digest_verify_final);
DISPATCH_RSASIG_FN(get_ctx_params);
DISPATCH_RSASIG_FN(set_ctx_params);
DISPATCH_RSASIG_FN(gettable_ctx_params);
DISPATCH_RSASIG_FN(settable_ctx_params);

static void *p11prov_rsasig_newctx(void *provctx, const char *properties)
{
    P11PROV_CTX *ctx = (P11PROV_CTX *)provctx;
    P11PROV_SIG_CTX *sigctx;

    /* PKCS1.5 is the default, PSS set via padding params */
    sigctx = p11prov_sig_newctx(ctx, CKM_RSA_PKCS, properties);
    if (sigctx == NULL) {
        return NULL;
    }

    /* default PSS Params */
    sigctx->pss_params.hashAlg = CKM_SHA256;
    sigctx->pss_params.mgf = CKG_MGF1_SHA256;
    sigctx->pss_params.sLen = 32;

    return sigctx;
}

static int p11prov_rsasig_sign_init(void *ctx, void *provkey,
                                    const OSSL_PARAM params[])
{
    int ret;

    P11PROV_debug("rsa sign init (ctx=%p, key=%p, params=%p)", ctx, provkey,
                  params);

    ret = p11prov_sig_op_init(ctx, provkey, CKF_SIGN, NULL, params);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    return p11prov_rsasig_set_ctx_params(ctx, params);
}

static int p11prov_rsasig_sign(void *ctx, unsigned char *sig, size_t *siglen,
                               size_t sigsize, const unsigned char *tbs,
                               size_t tbslen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;

    P11PROV_debug("rsa sign (ctx=%p)", ctx);

    return p11prov_sig_operate(sigctx, sig, siglen, sigsize, (void *)tbs,
                               tbslen);
}

static int p11prov_rsasig_verify_init(void *ctx, void *provkey,
                                      const OSSL_PARAM params[])
{
    int ret;

    P11PROV_debug("rsa verify init (ctx=%p, key=%p, params=%p)", ctx, provkey,
                  params);

    ret = p11prov_sig_op_init(ctx, provkey, CKF_VERIFY, NULL, params);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    return p11prov_rsasig_set_ctx_params(ctx, params);
}

static int p11prov_rsasig_verify(void *ctx, const unsigned char *sig,
                                 size_t siglen, const unsigned char *tbs,
                                 size_t tbslen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;

    P11PROV_debug("rsa verify (ctx=%p)", ctx);

    return p11prov_sig_operate(sigctx, (void *)sig, NULL, siglen, (void *)tbs,
                               tbslen);
}

static int p11prov_rsasig_digest_sign_init(void *ctx, const char *digest,
                                           void *provkey,
                                           const OSSL_PARAM params[])
{
    int ret;

    P11PROV_debug("rsa digest sign init (ctx=%p, digest=%s, key=%p, params=%p)",
                  ctx, digest ? digest : "<NULL>", provkey, params);

    ret = p11prov_sig_op_init(ctx, provkey, CKF_SIGN, digest, params);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    return p11prov_rsasig_set_ctx_params(ctx, params);
}

static int p11prov_rsasig_digest_sign_update(void *ctx,
                                             const unsigned char *data,
                                             size_t datalen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;

    P11PROV_debug("rsa digest sign update (ctx=%p, data=%p, datalen=%zu)", ctx,
                  data, datalen);

    if (sigctx == NULL) {
        return RET_OSSL_ERR;
    }

    return p11prov_sig_digest_update(sigctx, (void *)data, datalen);
}

static int p11prov_rsasig_digest_sign_final(void *ctx, unsigned char *sig,
                                            size_t *siglen, size_t sigsize)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;

    P11PROV_debug(
        "rsa digest sign final (ctx=%p, sig=%p, siglen=%zu, "
        "sigsize=%zu)",
        ctx, sig, *siglen, sigsize);

    if (sigctx == NULL) {
        return RET_OSSL_ERR;
    }

    return p11prov_sig_digest_final(sigctx, sig, siglen, sigsize);
}

static int p11prov_rsasig_digest_verify_init(void *ctx, const char *digest,
                                             void *provkey,
                                             const OSSL_PARAM params[])
{
    int ret;

    P11PROV_debug("rsa digest verify init (ctx=%p, key=%p, params=%p)", ctx,
                  provkey, params);

    ret = p11prov_sig_op_init(ctx, provkey, CKF_VERIFY, digest, params);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    return p11prov_rsasig_set_ctx_params(ctx, params);
}

static int p11prov_rsasig_digest_verify_update(void *ctx,
                                               const unsigned char *data,
                                               size_t datalen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;

    P11PROV_debug("rsa digest verify update (ctx=%p, data=%p, datalen=%zu)",
                  ctx, data, datalen);

    if (sigctx == NULL) {
        return RET_OSSL_ERR;
    }

    return p11prov_sig_digest_update(sigctx, (void *)data, datalen);
}

static int p11prov_rsasig_digest_verify_final(void *ctx,
                                              const unsigned char *sig,
                                              size_t siglen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;

    P11PROV_debug("rsa digest verify final (ctx=%p, sig=%p, siglen=%zu)", ctx,
                  sig, siglen);

    if (sigctx == NULL) {
        return RET_OSSL_ERR;
    }

    return p11prov_sig_digest_final(sigctx, (void *)sig, NULL, siglen);
}

static struct {
    CK_MECHANISM_TYPE type;
    int ossl_id;
    const char *string;
} padding_map[] = {
    { CKM_RSA_X_509, RSA_NO_PADDING, OSSL_PKEY_RSA_PAD_MODE_NONE },
    { CKM_RSA_PKCS, RSA_PKCS1_PADDING, OSSL_PKEY_RSA_PAD_MODE_PKCSV15 },
    { CKM_RSA_X9_31, RSA_X931_PADDING, OSSL_PKEY_RSA_PAD_MODE_X931 },
    { CKM_RSA_PKCS_PSS, RSA_PKCS1_PSS_PADDING, OSSL_PKEY_RSA_PAD_MODE_PSS },
    { CK_UNAVAILABLE_INFORMATION, 0, NULL },
};

static int p11prov_rsasig_get_ctx_params(void *ctx, OSSL_PARAM *params)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    OSSL_PARAM *p;
    int ret;

    /* todo sig params:
        OSSL_SIGNATURE_PARAM_ALGORITHM_ID
     */

    P11PROV_debug("rsasig get ctx params (ctx=%p, params=%p)", ctx, params);

    if (params == NULL) {
        return RET_OSSL_OK;
    }

    p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_ALGORITHM_ID);
    if (p) {
        const P11PROV_MECH *mech = NULL;
        CK_RV result;

        switch (sigctx->mechtype) {
        case CKM_RSA_PKCS:
            result = p11prov_mech_by_mechanism(sigctx->digest, &mech);
            if (result != CKR_OK) {
                return RET_OSSL_ERR;
            }
            ret = OSSL_PARAM_set_octet_string(p, mech->der_rsa_algorithm_id,
                                              mech->der_rsa_algorithm_id_len);
            if (ret != RET_OSSL_OK) {
                return ret;
            }
            break;
        case CKM_RSA_X_509:
            return RET_OSSL_ERR;
        case CKM_RSA_PKCS_PSS:
            /* TODO */
            return RET_OSSL_ERR;
        }
    }

    p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_DIGEST);
    if (p) {
        const char *digest;
        CK_RV rv;

        rv = p11prov_digest_get_name(sigctx->digest, &digest);
        if (rv != CKR_OK) {
            P11PROV_raise(sigctx->provctx, rv, "Unavailable digest name");
            return RET_OSSL_ERR;
        }
        ret = OSSL_PARAM_set_utf8_string(p, digest);
        if (ret != RET_OSSL_OK) {
            return ret;
        }
    }

    p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_PAD_MODE);
    if (p) {
        ret = RET_OSSL_ERR;
        for (int i = 0; padding_map[i].string != NULL; i++) {
            if (padding_map[i].type == sigctx->mechtype) {
                switch (p->data_type) {
                case OSSL_PARAM_INTEGER:
                    ret = OSSL_PARAM_set_int(p, padding_map[i].ossl_id);
                    break;
                case OSSL_PARAM_UTF8_STRING:
                    ret = OSSL_PARAM_set_utf8_string(p, padding_map[i].string);
                    break;
                }
                break;
            }
        }
        if (ret != RET_OSSL_OK) {
            return ret;
        }
    }

    p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_MGF1_DIGEST);
    if (p) {
        const char *digest = p11prov_sig_mgf_name(sigctx->pss_params.mgf);
        if (!digest) {
            return RET_OSSL_ERR;
        }
        ret = OSSL_PARAM_set_utf8_string(p, digest);
        if (ret != RET_OSSL_OK) {
            return ret;
        }
    }

    return RET_OSSL_OK;
}

static int p11prov_rsasig_set_ctx_params(void *ctx, const OSSL_PARAM params[])
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    const OSSL_PARAM *p;
    int ret;

    P11PROV_debug("rsasig set ctx params (ctx=%p, params=%p)", sigctx, params);

    if (params == NULL) {
        return RET_OSSL_OK;
    }

    p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_DIGEST);
    if (p) {
        const char *digest = NULL;
        CK_RV rv;

        ret = OSSL_PARAM_get_utf8_string_ptr(p, &digest);
        if (ret != RET_OSSL_OK) {
            return ret;
        }
        P11PROV_debug("Set OSSL_SIGNATURE_PARAM_DIGEST to %s", digest);

        rv = p11prov_digest_get_by_name(digest, &sigctx->digest);
        if (rv != CKR_OK) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_DIGEST);
            return RET_OSSL_ERR;
        }
    }

    p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PAD_MODE);
    if (p) {
        CK_MECHANISM_TYPE mechtype = CK_UNAVAILABLE_INFORMATION;
        if (p->data_type == OSSL_PARAM_INTEGER) {
            int pad_mode;
            /* legacy pad mode number */
            ret = OSSL_PARAM_get_int(p, &pad_mode);
            if (ret != RET_OSSL_OK) {
                return ret;
            }
            P11PROV_debug("Set OSSL_SIGNATURE_PARAM_PAD_MODE to %d", pad_mode);
            for (int i = 0; padding_map[i].string != NULL; i++) {
                if (padding_map[i].ossl_id == pad_mode) {
                    mechtype = padding_map[i].type;
                    break;
                }
            }
        } else if (p->data_type == OSSL_PARAM_UTF8_STRING) {
            P11PROV_debug("Set OSSL_SIGNATURE_PARAM_PAD_MODE to %s",
                          p->data ? (const char *)p->data : "<NULL>");
            if (p->data) {
                for (int i = 0; padding_map[i].string != NULL; i++) {
                    if (strcmp(p->data, padding_map[i].string) == 0) {
                        mechtype = padding_map[i].type;
                        break;
                    }
                }
            }
        } else {
            return RET_OSSL_ERR;
        }
        if (mechtype == CK_UNAVAILABLE_INFORMATION) {
            ERR_raise(ERR_LIB_PROV, PROV_R_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE);
            return RET_OSSL_ERR;
        }
        sigctx->mechtype = mechtype;

        P11PROV_debug_mechanism(sigctx->provctx,
                                p11prov_obj_get_slotid(sigctx->key),
                                sigctx->mechtype);
    }

    p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PSS_SALTLEN);
    if (p) {
        int saltlen;
        if (sigctx->mechtype != CKM_RSA_PKCS_PSS) {
            ERR_raise_data(ERR_LIB_PROV, PROV_R_NOT_SUPPORTED,
                           "Can only be set if PSS Padding was first set.");
            return RET_OSSL_ERR;
        }

        if (p->data_type == OSSL_PARAM_INTEGER) {
            /* legacy saltlen number */
            ret = OSSL_PARAM_get_int(p, &saltlen);
            if (ret != RET_OSSL_OK) {
                return ret;
            }
            P11PROV_debug("Set OSSL_SIGNATURE_PARAM_PSS_SALTLEN to %d",
                          saltlen);
            sigctx->pss_params.sLen = saltlen;
        } else if (p->data_type == OSSL_PARAM_UTF8_STRING) {
            P11PROV_debug("Set OSSL_SIGNATURE_PARAM_PSS_SALTLEN to %s",
                          p->data ? (const char *)p->data : "<NULL>");
            if (strcmp(p->data, OSSL_PKEY_RSA_PSS_SALT_LEN_DIGEST) == 0) {
                ret = p11prov_rsasig_set_pss_saltlen_from_digest(sigctx);
                if (ret != RET_OSSL_OK) {
                    return ret;
                }
            } else if (strcmp(p->data, OSSL_PKEY_RSA_PSS_SALT_LEN_MAX) == 0) {
                ERR_raise_data(ERR_LIB_PROV, PROV_R_NOT_SUPPORTED,
                               "saltlen=max is unsupported.");
                return RET_OSSL_ERR;
            } else if (strcmp(p->data, OSSL_PKEY_RSA_PSS_SALT_LEN_AUTO) == 0) {
                ERR_raise_data(ERR_LIB_PROV, PROV_R_NOT_SUPPORTED,
                               "saltlen=auto is unsupported.");
                return RET_OSSL_ERR;
            } else {
                saltlen = atoi(p->data);
                if (saltlen == 0) {
                    return RET_OSSL_ERR;
                }
                sigctx->pss_params.sLen = saltlen;
            }
        } else {
            return RET_OSSL_ERR;
        }
    }

    p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_MGF1_DIGEST);
    if (p) {
        const char *digest = NULL;
        ret = OSSL_PARAM_get_utf8_string_ptr(p, &digest);
        if (ret != RET_OSSL_OK) {
            return ret;
        }
        P11PROV_debug("Set OSSL_SIGNATURE_PARAM_MGF1_DIGEST to %s", digest);

        sigctx->pss_params.mgf = p11prov_sig_map_mgf(digest);
        if (sigctx->pss_params.mgf == CK_UNAVAILABLE_INFORMATION) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_MGF1_MD);
            return RET_OSSL_ERR;
        }
    }

    return RET_OSSL_OK;
}

static const OSSL_PARAM *p11prov_rsasig_gettable_ctx_params(void *ctx,
                                                            void *prov)
{
    static const OSSL_PARAM params[] = {
        OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_ALGORITHM_ID, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, NULL, 0),
        OSSL_PARAM_END,
    };
    return params;
}

static const OSSL_PARAM *p11prov_rsasig_settable_ctx_params(void *ctx,
                                                            void *prov)
{
    static const OSSL_PARAM params[] = {
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, NULL, 0),
        /* TODO: support rsa_padding_mode */
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, NULL, 0),
        OSSL_PARAM_END,
    };
    return params;
}

const OSSL_DISPATCH p11prov_rsa_signature_functions[] = {
    DISPATCH_SIG_ELEM(rsasig, NEWCTX, newctx),
    DISPATCH_SIG_ELEM(sig, FREECTX, freectx),
    DISPATCH_SIG_ELEM(sig, DUPCTX, dupctx),
    DISPATCH_SIG_ELEM(rsasig, SIGN_INIT, sign_init),
    DISPATCH_SIG_ELEM(rsasig, SIGN, sign),
    DISPATCH_SIG_ELEM(rsasig, VERIFY_INIT, verify_init),
    DISPATCH_SIG_ELEM(rsasig, VERIFY, verify),
    DISPATCH_SIG_ELEM(rsasig, DIGEST_SIGN_INIT, digest_sign_init),
    DISPATCH_SIG_ELEM(rsasig, DIGEST_SIGN_UPDATE, digest_sign_update),
    DISPATCH_SIG_ELEM(rsasig, DIGEST_SIGN_FINAL, digest_sign_final),
    DISPATCH_SIG_ELEM(rsasig, DIGEST_VERIFY_INIT, digest_verify_init),
    DISPATCH_SIG_ELEM(rsasig, DIGEST_VERIFY_UPDATE, digest_verify_update),
    DISPATCH_SIG_ELEM(rsasig, DIGEST_VERIFY_FINAL, digest_verify_final),
    DISPATCH_SIG_ELEM(rsasig, GET_CTX_PARAMS, get_ctx_params),
    DISPATCH_SIG_ELEM(rsasig, GETTABLE_CTX_PARAMS, gettable_ctx_params),
    DISPATCH_SIG_ELEM(rsasig, SET_CTX_PARAMS, set_ctx_params),
    DISPATCH_SIG_ELEM(rsasig, SETTABLE_CTX_PARAMS, settable_ctx_params),
    { 0, NULL },
};

DISPATCH_ECDSA_FN(newctx);
DISPATCH_ECDSA_FN(sign_init);
DISPATCH_ECDSA_FN(sign);
DISPATCH_ECDSA_FN(verify_init);
DISPATCH_ECDSA_FN(verify);
DISPATCH_ECDSA_FN(digest_sign_init);
DISPATCH_ECDSA_FN(digest_sign_update);
DISPATCH_ECDSA_FN(digest_sign_final);
DISPATCH_ECDSA_FN(digest_verify_init);
DISPATCH_ECDSA_FN(digest_verify_update);
DISPATCH_ECDSA_FN(digest_verify_final);
DISPATCH_ECDSA_FN(get_ctx_params);
DISPATCH_ECDSA_FN(set_ctx_params);
DISPATCH_ECDSA_FN(gettable_ctx_params);
DISPATCH_ECDSA_FN(settable_ctx_params);

static void *p11prov_ecdsa_newctx(void *provctx, const char *properties)
{
    P11PROV_CTX *ctx = (P11PROV_CTX *)provctx;
    P11PROV_SIG_CTX *sigctx;

    sigctx = p11prov_sig_newctx(ctx, CKM_ECDSA, properties);
    if (sigctx == NULL) {
        return NULL;
    }

    return sigctx;
}

static int p11prov_ecdsa_sign_init(void *ctx, void *provkey,
                                   const OSSL_PARAM params[])
{
    int ret;

    P11PROV_debug("ecdsa sign init (ctx=%p, key=%p, params=%p)", ctx, provkey,
                  params);

    ret = p11prov_sig_op_init(ctx, provkey, CKF_SIGN, NULL, params);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    return p11prov_ecdsa_set_ctx_params(ctx, params);
}

/* The raw signature is concatenated r | s padded to the field sizes */
#define P11PROV_MAX_RAW_ECC_SIG_SIZE (2 * (OPENSSL_ECC_MAX_FIELD_BITS + 7) / 8)

static int convert_ecdsa_raw_to_der(const unsigned char *raw, size_t rawlen,
                                    unsigned char *der, size_t *derlen,
                                    size_t dersize)
{
    const CK_ULONG fieldlen = rawlen / 2;
    ECDSA_SIG *ecdsasig;
    BIGNUM *r, *s;
    int ret = RET_OSSL_ERR;

    ecdsasig = ECDSA_SIG_new();
    if (ecdsasig == NULL) {
        return RET_OSSL_ERR;
    }

    r = BN_bin2bn(&raw[0], fieldlen, NULL);
    s = BN_bin2bn(&raw[fieldlen], fieldlen, NULL);
    ret = ECDSA_SIG_set0(ecdsasig, r, s);
    if (ret == RET_OSSL_OK) {
        *derlen = i2d_ECDSA_SIG(ecdsasig, NULL);
        if (*derlen <= dersize) {
            i2d_ECDSA_SIG(ecdsasig, &der);
            ret = RET_OSSL_OK;
        }
    } else {
        BN_clear_free(r);
        BN_clear_free(s);
    }

    ECDSA_SIG_free(ecdsasig);
    return ret;
}

static int convert_ecdsa_der_to_raw(const unsigned char *der, size_t derlen,
                                    unsigned char *raw, size_t rawlen,
                                    CK_ULONG fieldlen)
{
    ECDSA_SIG *ecdsasig;
    const BIGNUM *r, *s;

    if (fieldlen == CK_UNAVAILABLE_INFORMATION) {
        return RET_OSSL_ERR;
    }
    if (rawlen < 2 * fieldlen) {
        return RET_OSSL_ERR;
    }

    ecdsasig = d2i_ECDSA_SIG(NULL, &der, derlen);
    if (ecdsasig == NULL) {
        return RET_OSSL_ERR;
    }

    ECDSA_SIG_get0(ecdsasig, &r, &s);
    BN_bn2binpad(r, &raw[0], fieldlen);
    BN_bn2binpad(s, &raw[fieldlen], fieldlen);
    ECDSA_SIG_free(ecdsasig);
    return RET_OSSL_OK;
}

static int p11prov_ecdsa_sign(void *ctx, unsigned char *sig, size_t *siglen,
                              size_t sigsize, const unsigned char *tbs,
                              size_t tbslen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    unsigned char raw[P11PROV_MAX_RAW_ECC_SIG_SIZE];
    size_t rawlen;
    int ret;

    P11PROV_debug("ecdsa sign (ctx=%p)", ctx);
    if (sig == NULL || sigsize == 0) {
        return p11prov_sig_operate(sigctx, 0, siglen, 0, (void *)tbs, tbslen);
    }

    ret = p11prov_sig_operate(sigctx, raw, &rawlen, sizeof(raw), (void *)tbs,
                              tbslen);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    ret = convert_ecdsa_raw_to_der(raw, rawlen, sig, siglen, sigsize);
    OPENSSL_cleanse(raw, rawlen);
    return ret;
}

static int p11prov_ecdsa_verify_init(void *ctx, void *provkey,
                                     const OSSL_PARAM params[])
{
    int ret;

    P11PROV_debug("ecdsa verify init (ctx=%p, key=%p, params=%p)", ctx, provkey,
                  params);

    ret = p11prov_sig_op_init(ctx, provkey, CKF_VERIFY, NULL, params);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    return p11prov_ecdsa_set_ctx_params(ctx, params);
}

static int p11prov_ecdsa_verify(void *ctx, const unsigned char *sig,
                                size_t siglen, const unsigned char *tbs,
                                size_t tbslen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    unsigned char raw[P11PROV_MAX_RAW_ECC_SIG_SIZE];
    CK_ULONG flen = p11prov_obj_get_key_size(sigctx->key);
    int ret;

    P11PROV_debug("ecdsa verify (ctx=%p)", ctx);

    ret = convert_ecdsa_der_to_raw(sig, siglen, raw, sizeof(raw), flen);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    ret = p11prov_sig_operate(sigctx, (void *)raw, NULL, 2 * flen, (void *)tbs,
                              tbslen);
    OPENSSL_cleanse(raw, 2 * flen);
    return ret;
}

static int p11prov_ecdsa_digest_sign_init(void *ctx, const char *digest,
                                          void *provkey,
                                          const OSSL_PARAM params[])
{
    int ret;

    P11PROV_debug(
        "ecdsa digest sign init (ctx=%p, digest=%s, key=%p, params=%p)", ctx,
        digest ? digest : "<NULL>", provkey, params);

    ret = p11prov_sig_op_init(ctx, provkey, CKF_SIGN, digest, params);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    return p11prov_ecdsa_set_ctx_params(ctx, params);
}

static int p11prov_ecdsa_digest_sign_update(void *ctx,
                                            const unsigned char *data,
                                            size_t datalen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;

    P11PROV_debug("ecdsa digest sign update (ctx=%p, data=%p, datalen=%zu)",
                  ctx, data, datalen);

    if (sigctx == NULL) {
        return RET_OSSL_ERR;
    }

    return p11prov_sig_digest_update(sigctx, (void *)data, datalen);
}

static int p11prov_ecdsa_digest_sign_final(void *ctx, unsigned char *sig,
                                           size_t *siglen, size_t sigsize)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    unsigned char raw[P11PROV_MAX_RAW_ECC_SIG_SIZE];
    size_t rawlen;
    int ret;

    P11PROV_debug(
        "ecdsa digest sign final (ctx=%p, sig=%p, siglen=%zu, "
        "sigsize=%zu)",
        ctx, sig, *siglen, sigsize);

    if (sigctx == NULL) {
        return RET_OSSL_ERR;
    }
    if (sig == NULL || sigsize == 0) {
        return p11prov_sig_digest_final(sigctx, 0, siglen, 0);
    }

    ret = p11prov_sig_digest_final(sigctx, raw, &rawlen, sizeof(raw));
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    ret = convert_ecdsa_raw_to_der(raw, rawlen, sig, siglen, sigsize);
    OPENSSL_cleanse(raw, rawlen);
    return ret;
}

static int p11prov_ecdsa_digest_verify_init(void *ctx, const char *digest,
                                            void *provkey,
                                            const OSSL_PARAM params[])
{
    int ret;

    P11PROV_debug("ecdsa digest verify init (ctx=%p, key=%p, params=%p)", ctx,
                  provkey, params);

    ret = p11prov_sig_op_init(ctx, provkey, CKF_VERIFY, digest, params);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    return p11prov_ecdsa_set_ctx_params(ctx, params);
}

static int p11prov_ecdsa_digest_verify_update(void *ctx,
                                              const unsigned char *data,
                                              size_t datalen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;

    P11PROV_debug("ecdsa digest verify update (ctx=%p, data=%p, datalen=%zu)",
                  ctx, data, datalen);

    if (sigctx == NULL) {
        return RET_OSSL_ERR;
    }

    return p11prov_sig_digest_update(sigctx, (void *)data, datalen);
}

static int p11prov_ecdsa_digest_verify_final(void *ctx,
                                             const unsigned char *sig,
                                             size_t siglen)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    unsigned char raw[P11PROV_MAX_RAW_ECC_SIG_SIZE];
    CK_ULONG flen;
    int ret;

    P11PROV_debug("ecdsa digest verify final (ctx=%p, sig=%p, siglen=%zu)", ctx,
                  sig, siglen);

    if (sigctx == NULL) {
        return RET_OSSL_ERR;
    }

    flen = p11prov_obj_get_key_size(sigctx->key);

    ret = convert_ecdsa_der_to_raw(sig, siglen, raw, sizeof(raw), flen);
    if (ret != RET_OSSL_OK) {
        return ret;
    }

    ret = p11prov_sig_digest_final(sigctx, (void *)raw, NULL, 2 * flen);
    OPENSSL_cleanse(raw, 2 * flen);
    return ret;
}

static int p11prov_ecdsa_get_ctx_params(void *ctx, OSSL_PARAM *params)
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    OSSL_PARAM *p;
    int ret;

    /* todo sig params:
        OSSL_SIGNATURE_PARAM_ALGORITHM_ID
     */

    P11PROV_debug("ecdsa get ctx params (ctx=%p, params=%p)", ctx, params);

    p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_ALGORITHM_ID);
    if (p) {
        const P11PROV_MECH *mech = NULL;
        CK_RV result;

        switch (sigctx->mechtype) {
        case CKM_ECDSA:
            result = p11prov_mech_by_mechanism(sigctx->digest, &mech);
            if (result != CKR_OK) {
                return RET_OSSL_ERR;
            }
            ret = OSSL_PARAM_set_octet_string(p, mech->der_ecdsa_algorithm_id,
                                              mech->der_ecdsa_algorithm_id_len);
            if (ret != RET_OSSL_OK) {
                return ret;
            }
            break;
        default:
            return RET_OSSL_ERR;
        }
    }

    p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_DIGEST_SIZE);
    if (p) {
        size_t digest_size;
        CK_RV rv;

        rv = p11prov_digest_get_digest_size(sigctx->digest, &digest_size);
        if (rv != CKR_OK) {
            P11PROV_raise(sigctx->provctx, rv, "Unavailable digest size");
            return RET_OSSL_ERR;
        }
        ret = OSSL_PARAM_set_size_t(p, digest_size);
        if (ret != RET_OSSL_OK) {
            return ret;
        }
    }

    p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_DIGEST);
    if (p) {
        const char *digest;
        CK_RV rv;

        rv = p11prov_digest_get_name(sigctx->digest, &digest);
        if (rv != CKR_OK) {
            P11PROV_raise(sigctx->provctx, rv, "Unavailable digest name");
            return RET_OSSL_ERR;
        }
        ret = OSSL_PARAM_set_utf8_string(p, digest);
        if (ret != RET_OSSL_OK) {
            return ret;
        }
    }

    return RET_OSSL_OK;
}

static int p11prov_ecdsa_set_ctx_params(void *ctx, const OSSL_PARAM params[])
{
    P11PROV_SIG_CTX *sigctx = (P11PROV_SIG_CTX *)ctx;
    const OSSL_PARAM *p;
    int ret;

    P11PROV_debug("ecdsa set ctx params (ctx=%p, params=%p)", sigctx, params);

    if (params == NULL) {
        return RET_OSSL_OK;
    }

    p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_DIGEST);
    if (p) {
        const char *digest = NULL;
        CK_RV rv;

        ret = OSSL_PARAM_get_utf8_string_ptr(p, &digest);
        if (ret != RET_OSSL_OK) {
            return ret;
        }
        P11PROV_debug("Set OSSL_SIGNATURE_PARAM_DIGEST to %s", digest);

        rv = p11prov_digest_get_by_name(digest, &sigctx->digest);
        if (rv != CKR_OK) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_DIGEST);
            return RET_OSSL_ERR;
        }
    }

    return RET_OSSL_ERR;
}

static const OSSL_PARAM *p11prov_ecdsa_gettable_ctx_params(void *ctx,
                                                           void *prov)
{
    static const OSSL_PARAM params[] = {
        OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_ALGORITHM_ID, NULL, 0),
        OSSL_PARAM_size_t(OSSL_SIGNATURE_PARAM_DIGEST_SIZE, NULL),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
        OSSL_PARAM_END,
    };
    return params;
}

static const OSSL_PARAM *p11prov_ecdsa_settable_ctx_params(void *ctx,
                                                           void *prov)
{
    static const OSSL_PARAM params[] = {
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
        OSSL_PARAM_END,
    };
    return params;
}

const OSSL_DISPATCH p11prov_ecdsa_signature_functions[] = {
    DISPATCH_SIG_ELEM(ecdsa, NEWCTX, newctx),
    DISPATCH_SIG_ELEM(sig, FREECTX, freectx),
    DISPATCH_SIG_ELEM(sig, DUPCTX, dupctx),
    DISPATCH_SIG_ELEM(ecdsa, SIGN_INIT, sign_init),
    DISPATCH_SIG_ELEM(ecdsa, SIGN, sign),
    DISPATCH_SIG_ELEM(ecdsa, VERIFY_INIT, verify_init),
    DISPATCH_SIG_ELEM(ecdsa, VERIFY, verify),
    DISPATCH_SIG_ELEM(ecdsa, DIGEST_SIGN_INIT, digest_sign_init),
    DISPATCH_SIG_ELEM(ecdsa, DIGEST_SIGN_UPDATE, digest_sign_update),
    DISPATCH_SIG_ELEM(ecdsa, DIGEST_SIGN_FINAL, digest_sign_final),
    DISPATCH_SIG_ELEM(ecdsa, DIGEST_VERIFY_INIT, digest_verify_init),
    DISPATCH_SIG_ELEM(ecdsa, DIGEST_VERIFY_UPDATE, digest_verify_update),
    DISPATCH_SIG_ELEM(ecdsa, DIGEST_VERIFY_FINAL, digest_verify_final),
    DISPATCH_SIG_ELEM(ecdsa, GET_CTX_PARAMS, get_ctx_params),
    DISPATCH_SIG_ELEM(ecdsa, GETTABLE_CTX_PARAMS, gettable_ctx_params),
    DISPATCH_SIG_ELEM(ecdsa, SET_CTX_PARAMS, set_ctx_params),
    DISPATCH_SIG_ELEM(ecdsa, SETTABLE_CTX_PARAMS, settable_ctx_params),
    { 0, NULL },
};
