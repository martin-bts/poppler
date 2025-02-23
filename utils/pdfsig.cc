//========================================================================
//
// pdfsig.cc
//
// This file is licensed under the GPLv2 or later
//
// Copyright 2015 André Guerreiro <aguerreiro1985@gmail.com>
// Copyright 2015 André Esser <bepandre@hotmail.com>
// Copyright 2015, 2017-2022 Albert Astals Cid <aacid@kde.org>
// Copyright 2016 Markus Kilås <digital@markuspage.com>
// Copyright 2017, 2019 Hans-Ulrich Jüttner <huj@froreich-bioscientia.de>
// Copyright 2017, 2019 Adrian Johnson <ajohnson@redneon.com>
// Copyright 2018 Chinmoy Ranjan Pradhan <chinmoyrp65@protonmail.com>
// Copyright 2019 Alexey Pavlov <alexpux@gmail.com>
// Copyright 2019 Oliver Sander <oliver.sander@tu-dresden.de>
// Copyright 2019 Nelson Efrain A. Cruz <neac03@gmail.com>
// Copyright 2021 Georgiy Sgibnev <georgiy@sgibnev.com>. Work sponsored by lab50.net.
// Copyright 2021 Theofilos Intzoglou <int.teo@gmail.com>
//
//========================================================================

#include "config.h"
#include <poppler-config.h>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <hasht.h>
#include <fstream>
#include <random>
#include "parseargs.h"
#include "Object.h"
#include "Array.h"
#include "goo/gbasename.h"
#include "Page.h"
#include "PDFDoc.h"
#include "PDFDocFactory.h"
#include "Error.h"
#include "GlobalParams.h"
#include "SignatureHandler.h"
#include "SignatureInfo.h"
#include "Win32Console.h"
#include "numberofcharacters.h"
#include "UTF.h"
#include <libgen.h>

static const char *getReadableSigState(SignatureValidationStatus sig_vs)
{
    switch (sig_vs) {
    case SIGNATURE_VALID:
        return "Signature is Valid.";

    case SIGNATURE_INVALID:
        return "Signature is Invalid.";

    case SIGNATURE_DIGEST_MISMATCH:
        return "Digest Mismatch.";

    case SIGNATURE_DECODING_ERROR:
        return "Document isn't signed or corrupted data.";

    case SIGNATURE_NOT_VERIFIED:
        return "Signature has not yet been verified.";

    default:
        return "Unknown Validation Failure.";
    }
}

static const char *getReadableCertState(CertificateValidationStatus cert_vs)
{
    switch (cert_vs) {
    case CERTIFICATE_TRUSTED:
        return "Certificate is Trusted.";

    case CERTIFICATE_UNTRUSTED_ISSUER:
        return "Certificate issuer isn't Trusted.";

    case CERTIFICATE_UNKNOWN_ISSUER:
        return "Certificate issuer is unknown.";

    case CERTIFICATE_REVOKED:
        return "Certificate has been Revoked.";

    case CERTIFICATE_EXPIRED:
        return "Certificate has Expired";

    case CERTIFICATE_NOT_VERIFIED:
        return "Certificate has not yet been verified.";

    default:
        return "Unknown issue with Certificate or corrupted data.";
    }
}

static char *getReadableTime(time_t unix_time)
{
    char *time_str = (char *)gmalloc(64);
    strftime(time_str, 64, "%b %d %Y %H:%M:%S", localtime(&unix_time));
    return time_str;
}

static bool dumpSignature(int sig_num, int sigCount, FormFieldSignature *s, const char *filename)
{
    const GooString *signature = s->getSignature();
    if (!signature) {
        printf("Cannot dump signature #%d\n", sig_num);
        return false;
    }

    const int sigCountLength = numberOfCharacters(sigCount);
    // We want format to be {0:s}.sig{1:Xd} where X is sigCountLength
    // since { is the magic character to replace things we need to put it twice where
    // we don't want it to be replaced
    GooString *format = GooString::format("{{0:s}}.sig{{1:{0:d}d}}", sigCountLength);
    GooString *path = GooString::format(format->c_str(), gbasename(filename).c_str(), sig_num);
    printf("Signature #%d (%u bytes) => %s\n", sig_num, signature->getLength(), path->c_str());
    std::ofstream outfile(path->c_str(), std::ofstream::binary);
    outfile.write(signature->c_str(), signature->getLength());
    outfile.close();
    delete format;
    delete path;

    return true;
}

static GooString nssDir;
static GooString nssPassword;
static char ownerPassword[33] = "\001";
static char userPassword[33] = "\001";
static bool printVersion = false;
static bool printHelp = false;
static bool dontVerifyCert = false;
static bool noOCSPRevocationCheck = false;
static bool dumpSignatures = false;
static bool etsiCAdESdetached = false;
static int signatureNumber = 0;
static char certNickname[256] = "";
static char password[256] = "";
static char digestName[256] = "SHA256";
static GooString reason;
static bool listNicknames = false;
static bool addNewSignature = false;
static bool useAIACertFetch = false;
static GooString newSignatureFieldName;

static const ArgDesc argDesc[] = { { "-nssdir", argGooString, &nssDir, 0, "path to directory of libnss3 database" },
                                   { "-nss-pwd", argGooString, &nssPassword, 0, "password to access the NSS database (if any)" },
                                   { "-nocert", argFlag, &dontVerifyCert, 0, "don't perform certificate validation" },
                                   { "-no-ocsp", argFlag, &noOCSPRevocationCheck, 0, "don't perform online OCSP certificate revocation check" },
                                   { "-aia", argFlag, &useAIACertFetch, 0, "use Authority Information Access (AIA) extension for certificate fetching" },
                                   { "-dump", argFlag, &dumpSignatures, 0, "dump all signatures into current directory" },
                                   { "-add-signature", argFlag, &addNewSignature, 0, "adds a new signature to the document" },
                                   { "-new-signature-field-name", argGooString, &newSignatureFieldName, 0, "field name used for the newly added signature. A random ID will be used if empty" },
                                   { "-sign", argInt, &signatureNumber, 0, "sign the document in the signature field with the given number" },
                                   { "-etsi", argFlag, &etsiCAdESdetached, 0, "create a signature of type ETSI.CAdES.detached instead of adbe.pkcs7.detached" },
                                   { "-nick", argString, &certNickname, 256, "use the certificate with the given nickname for signing" },
                                   { "-kpw", argString, &password, 256, "password for the signing key (might be missing if the key isn't password protected)" },
                                   { "-digest", argString, &digestName, 256, "name of the digest algorithm (default: SHA256)" },
                                   { "-reason", argGooString, &reason, 0, "reason for signing (default: no reason given)" },
                                   { "-list-nicks", argFlag, &listNicknames, 0, "list available nicknames in the NSS database" },
                                   { "-opw", argString, ownerPassword, sizeof(ownerPassword), "owner password (for encrypted files)" },
                                   { "-upw", argString, userPassword, sizeof(userPassword), "user password (for encrypted files)" },
                                   { "-v", argFlag, &printVersion, 0, "print copyright and version info" },
                                   { "-h", argFlag, &printHelp, 0, "print usage information" },
                                   { "-help", argFlag, &printHelp, 0, "print usage information" },
                                   { "-?", argFlag, &printHelp, 0, "print usage information" },
                                   {} };

static void print_version_usage(bool usage)
{
    fprintf(stderr, "pdfsig version %s\n", PACKAGE_VERSION);
    fprintf(stderr, "%s\n", popplerCopyright);
    fprintf(stderr, "%s\n", xpdfCopyright);
    if (usage) {
        printUsage("pdfsig", "<PDF-file> [<output-file>]", argDesc);
    }
}

static std::vector<std::unique_ptr<X509CertificateInfo>> getAvailableSigningCertificates(bool *error)
{
    bool wrongPassword = false;
    bool passwordNeeded = false;
    auto passwordCallback = [&passwordNeeded, &wrongPassword](const char *) -> char * {
        static bool firstTime = true;
        if (!firstTime) {
            wrongPassword = true;
            return nullptr;
        }
        firstTime = false;
        if (nssPassword.getLength() > 0) {
            return strdup(nssPassword.c_str());
        } else {
            passwordNeeded = true;
            return nullptr;
        }
    };
    SignatureHandler::setNSSPasswordCallback(passwordCallback);
    std::vector<std::unique_ptr<X509CertificateInfo>> vCerts = SignatureHandler::getAvailableSigningCertificates();
    SignatureHandler::setNSSPasswordCallback({});
    if (passwordNeeded) {
        *error = true;
        printf("Password is needed to access the NSS database.\n");
        printf("\tPlease provide one with -nss-pwd.\n");
        return {};
    }
    if (wrongPassword) {
        *error = true;
        printf("Password was not accepted to open the NSS database.\n");
        printf("\tPlease provide the correct one with -nss-pwd.\n");
        return {};
    }

    *error = false;
    return vCerts;
}

int main(int argc, char *argv[])
{
    char *time_str = nullptr;
    globalParams = std::make_unique<GlobalParams>();

    Win32Console win32Console(&argc, &argv);

    const bool ok = parseArgs(argDesc, &argc, argv);

    if (!ok) {
        print_version_usage(true);
        return 99;
    }

    if (printVersion) {
        print_version_usage(false);
        return 0;
    }

    if (printHelp) {
        print_version_usage(true);
        return 0;
    }

    SignatureHandler::setNSSDir(nssDir);

    if (listNicknames) {
        bool getCertsError;
        const std::vector<std::unique_ptr<X509CertificateInfo>> vCerts = getAvailableSigningCertificates(&getCertsError);
        if (getCertsError) {
            return 2;
        } else {
            if (vCerts.empty()) {
                printf("There are no certificates available.\n");
            } else {
                printf("Certificate nicknames available:\n");
                for (auto &cert : vCerts) {
                    const GooString &nick = cert->getNickName();
                    printf("%s\n", nick.c_str());
                }
            }
        }
        return 0;
    }

    if (argc < 2) {
        // no filename was given
        print_version_usage(true);
        return 99;
    }

    std::unique_ptr<GooString> fileName = std::make_unique<GooString>(argv[1]);

    std::unique_ptr<GooString> ownerPW, userPW;
    if (ownerPassword[0] != '\001') {
        ownerPW = std::make_unique<GooString>(ownerPassword);
    }
    if (userPassword[0] != '\001') {
        userPW = std::make_unique<GooString>(userPassword);
    }
    // open PDF file
    std::unique_ptr<PDFDoc> doc(PDFDocFactory().createPDFDoc(*fileName, ownerPW.get(), userPW.get()));

    if (!doc->isOk()) {
        return 1;
    }

    if (addNewSignature && signatureNumber > 0) {
        // incompatible options
        print_version_usage(true);
        return 99;
    }

    if (addNewSignature) {
        if (argc == 2) {
            fprintf(stderr, "An output filename for the signed document must be given\n");
            return 2;
        }

        if (strlen(certNickname) == 0) {
            printf("A nickname of the signing certificate must be given\n");
            return 2;
        }

        if (etsiCAdESdetached) {
            printf("-etsi is not supported yet with -add-signature\n");
            printf("Please file a bug report if this is important for you\n");
            return 2;
        }

        if (digestName != std::string("SHA256")) {
            printf("Only digest SHA256 is supported at the moment with -add-signature\n");
            printf("Please file a bug report if this is important for you\n");
            return 2;
        }

        if (doc->getPage(1) == nullptr) {
            printf("Error getting first page of the document.\n");
            return 2;
        }

        bool getCertsError;
        // We need to call this otherwise NSS spins forever
        getAvailableSigningCertificates(&getCertsError);
        if (getCertsError) {
            return 2;
        }

        const char *pw = (strlen(password) == 0) ? nullptr : password;
        const auto rs = std::unique_ptr<GooString>(reason.toStr().empty() ? nullptr : utf8ToUtf16WithBom(reason.toStr()));

        if (newSignatureFieldName.getLength() == 0) {
            // Create a random field name, it could be anything but 32 hex numbers should
            // hopefully give us something that is not already in the document
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(1, 15);
            for (int i = 0; i < 32; ++i) {
                const int value = distrib(gen);
                newSignatureFieldName.append(value < 10 ? 48 + value : 65 + (value - 10));
            }
        }

        // We don't provide a way to customize the UI from pdfsig for now
        const bool success = doc->sign(argv[2], certNickname, pw, newSignatureFieldName.copy(), /*page*/ 1,
                                       /*rect */ { 0, 0, 0, 0 }, /*signatureText*/ {}, /*signatureTextLeft*/ {}, /*fontSize */ 0,
                                       /*fontColor*/ {}, /*borderWidth*/ 0, /*borderColor*/ {}, /*backgroundColor*/ {}, rs.get(), /* location */ nullptr, /* image path */ "", ownerPW.get(), userPW.get());
        return success ? 0 : 3;
    }

    const std::vector<FormFieldSignature *> signatures = doc->getSignatureFields();
    const unsigned int sigCount = signatures.size();

    if (signatureNumber > 0) {
        // We are signing an existing signature field
        if (argc == 2) {
            fprintf(stderr, "An output filename for the signed document must be given\n");
            return 2;
        }

        if (signatureNumber > static_cast<int>(sigCount)) {
            printf("File '%s' does not contain a signature with number %d\n", fileName->c_str(), signatureNumber);
            return 2;
        }

        if (strlen(certNickname) == 0) {
            printf("A nickname of the signing certificate must be given\n");
            return 2;
        }

        bool getCertsError;
        // We need to call this otherwise NSS spins forever
        getAvailableSigningCertificates(&getCertsError);
        if (getCertsError) {
            return 2;
        }

        FormFieldSignature *ffs = signatures.at(signatureNumber - 1);
        Goffset file_size = 0;
        GooString *sig = ffs->getCheckedSignature(&file_size);
        if (sig) {
            delete sig;
            printf("Signature number %d is already signed\n", signatureNumber);
            return 2;
        }
        if (etsiCAdESdetached)
            ffs->setSignatureType(ETSI_CAdES_detached);
        const char *pw = (strlen(password) == 0) ? nullptr : password;
        const auto rs = std::unique_ptr<GooString>(reason.toStr().empty() ? nullptr : utf8ToUtf16WithBom(reason.toStr()));
        if (ffs->getNumWidgets() != 1) {
            printf("Unexpected number of widgets for the signature: %d\n", ffs->getNumWidgets());
            return 2;
        }
        FormWidgetSignature *fws = static_cast<FormWidgetSignature *>(ffs->getWidget(0));
        const bool success = fws->signDocument(argv[2], certNickname, digestName, pw, rs.get());
        return success ? 0 : 3;
    }

    if (argc > 2) {
        // We are not signing and more than 1 filename was given
        print_version_usage(true);
        return 99;
    }

    if (sigCount >= 1) {
        if (dumpSignatures) {
            printf("Dumping Signatures: %u\n", sigCount);
            for (unsigned int i = 0; i < sigCount; i++) {
                const bool dumpingOk = dumpSignature(i, sigCount, signatures.at(i), fileName->c_str());
                if (!dumpingOk) {
                    return 3;
                }
            }
            return 0;
        } else {
            printf("Digital Signature Info of: %s\n", fileName->c_str());
        }
    } else {
        printf("File '%s' does not contain any signatures\n", fileName->c_str());
        return 2;
    }

    for (unsigned int i = 0; i < sigCount; i++) {
        const SignatureInfo *sig_info = signatures.at(i)->validateSignature(!dontVerifyCert, false, -1 /* now */, !noOCSPRevocationCheck, useAIACertFetch);
        printf("Signature #%u:\n", i + 1);
        printf("  - Signer Certificate Common Name: %s\n", sig_info->getSignerName());
        printf("  - Signer full Distinguished Name: %s\n", sig_info->getSubjectDN());
        printf("  - Signing Time: %s\n", time_str = getReadableTime(sig_info->getSigningTime()));
        printf("  - Signing Hash Algorithm: ");
        switch (sig_info->getHashAlgorithm()) {
        case HASH_AlgMD2:
            printf("MD2\n");
            break;
        case HASH_AlgMD5:
            printf("MD5\n");
            break;
        case HASH_AlgSHA1:
            printf("SHA1\n");
            break;
        case HASH_AlgSHA256:
            printf("SHA-256\n");
            break;
        case HASH_AlgSHA384:
            printf("SHA-384\n");
            break;
        case HASH_AlgSHA512:
            printf("SHA-512\n");
            break;
        case HASH_AlgSHA224:
            printf("SHA-224\n");
            break;
        default:
            printf("unknown\n");
        }
        printf("  - Signature Type: ");
        switch (signatures.at(i)->getSignatureType()) {
        case adbe_pkcs7_sha1:
            printf("adbe.pkcs7.sha1\n");
            break;
        case adbe_pkcs7_detached:
            printf("adbe.pkcs7.detached\n");
            break;
        case ETSI_CAdES_detached:
            printf("ETSI.CAdES.detached\n");
            break;
        default:
            printf("unknown\n");
        }
        std::vector<Goffset> ranges = signatures.at(i)->getSignedRangeBounds();
        if (ranges.size() == 4) {
            printf("  - Signed Ranges: [%lld - %lld], [%lld - %lld]\n", ranges[0], ranges[1], ranges[2], ranges[3]);
            Goffset checked_file_size;
            GooString *signature = signatures.at(i)->getCheckedSignature(&checked_file_size);
            if (signature && checked_file_size == ranges[3]) {
                printf("  - Total document signed\n");
            } else {
                printf("  - Not total document signed\n");
            }
            delete signature;
        }
        printf("  - Signature Validation: %s\n", getReadableSigState(sig_info->getSignatureValStatus()));
        gfree(time_str);
        if (sig_info->getSignatureValStatus() != SIGNATURE_VALID || dontVerifyCert) {
            continue;
        }
        printf("  - Certificate Validation: %s\n", getReadableCertState(sig_info->getCertificateValStatus()));
    }

    return 0;
}
