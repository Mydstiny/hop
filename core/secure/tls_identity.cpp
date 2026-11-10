#include "hop/tls_identity.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <array>
#include <cstdio>
#include <ctime>

namespace hop {
namespace {
struct Generation {
    mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context rng; mbedtls_pk_context key;
    mbedtls_x509write_cert cert; std::array<unsigned char,8192> keyPem{}, certPem{};
    Generation() { mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&rng); mbedtls_pk_init(&key); mbedtls_x509write_crt_init(&cert); }
    ~Generation() { mbedtls_platform_zeroize(keyPem.data(),keyPem.size()); mbedtls_x509write_crt_free(&cert); mbedtls_pk_free(&key); mbedtls_ctr_drbg_free(&rng); mbedtls_entropy_free(&entropy); }
};
bool utc(std::time_t value, char (&out)[15]) {
    std::tm result{};
#ifdef _WIN32
    if (gmtime_s(&result,&value) != 0) return false;
#else
    if (!gmtime_r(&value,&result)) return false;
#endif
    return std::strftime(out,sizeof(out),"%Y%m%d%H%M%S",&result)==14;
}
}
bool validate_tls_identity(const std::string& pem, const std::string& key) {
    if (pem.empty() || key.empty() || pem.size()>65536 || key.size()>65536 ||
        pem.find('\0')!=std::string::npos || key.find('\0')!=std::string::npos) return false;
    Generation g; mbedtls_x509_crt cert; mbedtls_x509_crt_init(&cert);
    constexpr char label[]="Hop device identity validation";
    int rc=mbedtls_ctr_drbg_seed(&g.rng,mbedtls_entropy_func,&g.entropy,reinterpret_cast<const unsigned char*>(label),sizeof(label)-1);
    if (!rc) rc=mbedtls_x509_crt_parse(&cert,reinterpret_cast<const unsigned char*>(pem.c_str()),pem.size()+1);
    if (!rc && cert.next) rc=-1;
    if (!rc) rc=mbedtls_pk_parse_key(&g.key,reinterpret_cast<const unsigned char*>(key.c_str()),key.size()+1,nullptr,0,mbedtls_ctr_drbg_random,&g.rng);
    if (!rc) rc=mbedtls_pk_check_pair(&cert.pk,&g.key,mbedtls_ctr_drbg_random,&g.rng);
    mbedtls_x509_crt_free(&cert);
    return rc==0;
}
bool tls_certificate_fingerprint(const std::string& pem, std::string& hex) {
    hex.clear();
    if (pem.empty() || pem.size()>65536 || pem.find('\0')!=std::string::npos) return false;
    mbedtls_x509_crt cert; mbedtls_x509_crt_init(&cert);
    int rc=mbedtls_x509_crt_parse(&cert,reinterpret_cast<const unsigned char*>(pem.c_str()),pem.size()+1);
    unsigned char digest[32]{};
    if (!rc && !cert.next) rc=mbedtls_sha256(cert.raw.p,cert.raw.len,digest,0);
    else if (!rc) rc=-1;
    if (!rc) {
        static constexpr char digits[]="0123456789ABCDEF";
        hex.reserve(64);
        for (auto c:digest) { hex.push_back(digits[c>>4]); hex.push_back(digits[c&15]); }
    }
    mbedtls_platform_zeroize(digest,sizeof(digest)); mbedtls_x509_crt_free(&cert);
    return rc==0;
}
bool generate_tls_identity(GeneratedTlsIdentity& output) {
    output.certificate_pem.clear(); output.fingerprint_sha256.clear();
    if (!output.private_key_pem.empty()) { mbedtls_platform_zeroize(&output.private_key_pem[0],output.private_key_pem.size()); output.private_key_pem.clear(); }
    Generation g;
    constexpr char label[]="Hop device identity v2";
    int rc=mbedtls_ctr_drbg_seed(&g.rng,mbedtls_entropy_func,&g.entropy,reinterpret_cast<const unsigned char*>(label),sizeof(label)-1);
    if (!rc) rc=mbedtls_pk_setup(&g.key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (!rc) rc=mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(g.key),mbedtls_ctr_drbg_random,&g.rng);
    std::array<unsigned char,16> serial{};
    if (!rc) rc=mbedtls_ctr_drbg_random(&g.rng,serial.data(),serial.size());
    serial[0]=(serial[0]&0x7f)|1;
    mbedtls_x509write_crt_set_version(&g.cert,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&g.cert,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&g.cert,&g.key);
    mbedtls_x509write_crt_set_issuer_key(&g.cert,&g.key);
    if (!rc) rc=mbedtls_x509write_crt_set_serial_raw(&g.cert,serial.data(),serial.size());
    if (!rc) rc=mbedtls_x509write_crt_set_subject_name(&g.cert,"CN=Hop device");
    if (!rc) rc=mbedtls_x509write_crt_set_issuer_name(&g.cert,"CN=Hop device");
    const std::time_t now=std::time(nullptr); char before[15]{},after[15]{};
    if (now<=0 || !utc(now-300,before) || !utc(now+std::time_t(10LL*365*24*60*60),after)) return false;
    if (!rc) rc=mbedtls_x509write_crt_set_validity(&g.cert,before,after);
    if (!rc) rc=mbedtls_x509write_crt_set_basic_constraints(&g.cert,1,-1);
    if (!rc) rc=mbedtls_x509write_crt_set_key_usage(&g.cert,MBEDTLS_X509_KU_DIGITAL_SIGNATURE|MBEDTLS_X509_KU_KEY_CERT_SIGN);
    if (!rc) rc=mbedtls_pk_write_key_pem(&g.key,g.keyPem.data(),g.keyPem.size());
    if (!rc) rc=mbedtls_x509write_crt_pem(&g.cert,g.certPem.data(),g.certPem.size(),mbedtls_ctr_drbg_random,&g.rng);
    if (rc) return false;
    output.certificate_pem=reinterpret_cast<const char*>(g.certPem.data());
    output.private_key_pem=reinterpret_cast<const char*>(g.keyPem.data());
    if (!tls_certificate_fingerprint(output.certificate_pem,output.fingerprint_sha256)) {
        mbedtls_platform_zeroize(&output.private_key_pem[0],output.private_key_pem.size()); output.private_key_pem.clear(); output.certificate_pem.clear();
        return false;
    }
    return true;
}
} // namespace hop
