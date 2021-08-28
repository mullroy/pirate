// Copyright (c) 2009-2013 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/******************************************************************************
 * Copyright © 2014-2019 The SuperNET Developers.                             *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * SuperNET software, including this file may be copied, modified, propagated *
 * or distributed except according to the terms contained in the LICENSE file *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

#include "crypter.h"

#include "script/script.h"
#include "script/standard.h"
#include "streams.h"
#include "util.h"

#include <string>
#include <vector>
#include <boost/foreach.hpp>
#include <openssl/aes.h>
#include <openssl/evp.h>

using namespace libzcash;

bool CCrypter::SetKeyFromPassphrase(const SecureString& strKeyData, const std::vector<unsigned char>& chSalt, const unsigned int nRounds, const unsigned int nDerivationMethod)
{
    if (nRounds < 1 || chSalt.size() != WALLET_CRYPTO_SALT_SIZE)
        return false;

    int i = 0;
    if (nDerivationMethod == 0)
        i = EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha512(), &chSalt[0],
                          (unsigned char *)&strKeyData[0], strKeyData.size(), nRounds, chKey, chIV);

    if (i != (int)WALLET_CRYPTO_KEY_SIZE)
    {
        memory_cleanse(chKey, sizeof(chKey));
        memory_cleanse(chIV, sizeof(chIV));
        return false;
    }

    fKeySet = true;
    return true;
}

bool CCrypter::SetKey(const CKeyingMaterial& chNewKey, const std::vector<unsigned char>& chNewIV)
{
    if (chNewKey.size() != WALLET_CRYPTO_KEY_SIZE || chNewIV.size() != WALLET_CRYPTO_KEY_SIZE)
        return false;

    memcpy(&chKey[0], &chNewKey[0], sizeof chKey);
    memcpy(&chIV[0], &chNewIV[0], sizeof chIV);

    fKeySet = true;
    return true;
}

bool CCrypter::Encrypt(const CKeyingMaterial& vchPlaintext, std::vector<unsigned char> &vchCiphertext)
{
    if (!fKeySet)
        return false;

    // max ciphertext len for a n bytes of plaintext is
    // n + AES_BLOCK_SIZE - 1 bytes
    int nLen = vchPlaintext.size();
    int nCLen = nLen + AES_BLOCK_SIZE, nFLen = 0;
    vchCiphertext = std::vector<unsigned char> (nCLen);

    bool fOk = true;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    assert(ctx);
    if (fOk) fOk = EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, chKey, chIV) != 0;
    if (fOk) fOk = EVP_EncryptUpdate(ctx, &vchCiphertext[0], &nCLen, &vchPlaintext[0], nLen) != 0;
    if (fOk) fOk = EVP_EncryptFinal_ex(ctx, (&vchCiphertext[0]) + nCLen, &nFLen) != 0;
    EVP_CIPHER_CTX_free(ctx);

    if (!fOk) return false;

    vchCiphertext.resize(nCLen + nFLen);
    return true;
}

bool CCrypter::Decrypt(const std::vector<unsigned char>& vchCiphertext, CKeyingMaterial& vchPlaintext)
{
    if (!fKeySet)
        return false;

    // plaintext will always be equal to or lesser than length of ciphertext
    int nLen = vchCiphertext.size();
    int nPLen = nLen, nFLen = 0;

    vchPlaintext = CKeyingMaterial(nPLen);

    bool fOk = true;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    assert(ctx);
    if (fOk) fOk = EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, chKey, chIV) != 0;
    if (fOk) fOk = EVP_DecryptUpdate(ctx, &vchPlaintext[0], &nPLen, &vchCiphertext[0], nLen) != 0;
    if (fOk) fOk = EVP_DecryptFinal_ex(ctx, (&vchPlaintext[0]) + nPLen, &nFLen) != 0;
    EVP_CIPHER_CTX_free(ctx);

    if (!fOk) return false;

    vchPlaintext.resize(nPLen + nFLen);
    return true;
}


static bool EncryptSecret(const CKeyingMaterial& vMasterKey, const CKeyingMaterial &vchPlaintext, const uint256& nIV, std::vector<unsigned char> &vchCiphertext)
{
    CCrypter cKeyCrypter;
    std::vector<unsigned char> chIV(WALLET_CRYPTO_KEY_SIZE);
    memcpy(&chIV[0], &nIV, WALLET_CRYPTO_KEY_SIZE);
    if (!cKeyCrypter.SetKey(vMasterKey, chIV))
        return false;
    return cKeyCrypter.Encrypt(*((const CKeyingMaterial*)&vchPlaintext), vchCiphertext);
}

static bool DecryptSecret(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCiphertext, const uint256& nIV, CKeyingMaterial& vchPlaintext)
{
    CCrypter cKeyCrypter;
    std::vector<unsigned char> chIV(WALLET_CRYPTO_KEY_SIZE);
    memcpy(&chIV[0], &nIV, WALLET_CRYPTO_KEY_SIZE);
    if (!cKeyCrypter.SetKey(vMasterKey, chIV))
        return false;
    return cKeyCrypter.Decrypt(vchCiphertext, *((CKeyingMaterial*)&vchPlaintext));
}

static bool DecryptHDSeed(
    const CKeyingMaterial& vMasterKey,
    const std::vector<unsigned char>& vchCryptedSecret,
    const uint256& seedFp,
    HDSeed& seed)
{
    CKeyingMaterial vchSecret;

    // Use seed's fingerprint as IV
    // TODO: Handle IV properly when we make encryption a supported feature
    if(!DecryptSecret(vMasterKey, vchCryptedSecret, seedFp, vchSecret))
        return false;

    seed = HDSeed(vchSecret);
    return seed.Fingerprint() == seedFp;
}

static bool DecryptKey(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCryptedSecret, const CPubKey& vchPubKey, CKey& key)
{
    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, vchPubKey.GetHash(), vchSecret))
        return false;

    if (vchSecret.size() != 32)
        return false;

    key.Set(vchSecret.begin(), vchSecret.end(), vchPubKey.IsCompressed());
    return key.VerifyPubKey(vchPubKey);
}

static bool DecryptSproutSpendingKey(const CKeyingMaterial& vMasterKey,
                               const std::vector<unsigned char>& vchCryptedSecret,
                               const libzcash::SproutPaymentAddress& address,
                               libzcash::SproutSpendingKey& sk)
{
    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, address.GetHash(), vchSecret))
        return false;

    if (vchSecret.size() != libzcash::SerializedSproutSpendingKeySize)
        return false;

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> sk;
    return sk.address() == address;
}

static bool DecryptSaplingSpendingKey(const CKeyingMaterial& vMasterKey,
                               const std::vector<unsigned char>& vchCryptedSecret,
                               const uint256& extfvkFinger,
                               libzcash::SaplingExtendedSpendingKey& sk)
{
    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, extfvkFinger, vchSecret))
        return false;

    if (vchSecret.size() != ZIP32_XSK_SIZE)
        return false;

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> sk;
    return sk.ToXFVK().fvk.GetFingerprint() == extfvkFinger;
}

static bool DecryptSaplingExtendedFullViewingKey(const CKeyingMaterial& vMasterKey,
                               const std::vector<unsigned char>& vchCryptedSecret,
                               const uint256& extfvkFinger,
                               libzcash::SaplingExtendedFullViewingKey& extfvk)
{
    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, extfvkFinger, vchSecret))
        return false;

    if (vchSecret.size() != ZIP32_XFVK_SIZE)
        return false;

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> extfvk;
    return extfvk.fvk.GetFingerprint() == extfvkFinger;
}

bool CCryptoKeyStore::SetCrypted()
{
    LOCK2(cs_KeyStore, cs_SpendingKeyStore);
    if (fUseCrypto)
        return true;
    if (!(mapKeys.empty() && mapSproutSpendingKeys.empty() && mapSaplingSpendingKeys.empty()))
        return false;
    fUseCrypto = true;
    return true;
}

bool CCryptoKeyStore::Lock()
{
    if (!SetCrypted())
        return false;

    {
        LOCK(cs_KeyStore);
        vMasterKey.clear();
    }

    NotifyStatusChanged(this);
    return true;
}

bool CCryptoKeyStore::Unlock(const CKeyingMaterial& vMasterKeyIn)
{
    {
        LOCK2(cs_KeyStore, cs_SpendingKeyStore);
        if (!SetCrypted())
            return false;

        bool keyPass = false;
        bool keyFail = false;
        if (!cryptedHDSeed.first.IsNull()) {
            HDSeed seed;
            if (!DecryptHDSeed(vMasterKeyIn, cryptedHDSeed.second, cryptedHDSeed.first, seed))
            {
                keyFail = true;
            } else {
                keyPass = true;
            }
        }
        CryptedKeyMap::const_iterator mi = mapCryptedKeys.begin();
        for (; mi != mapCryptedKeys.end(); ++mi)
        {
            const CPubKey &vchPubKey = (*mi).second.first;
            const std::vector<unsigned char> &vchCryptedSecret = (*mi).second.second;
            CKey key;
            if (!DecryptKey(vMasterKeyIn, vchCryptedSecret, vchPubKey, key))
            {
                keyFail = true;
                break;
            }
            keyPass = true;
            if (fDecryptionThoroughlyChecked)
                break;
        }
        CryptedSproutSpendingKeyMap::const_iterator miSprout = mapCryptedSproutSpendingKeys.begin();
        for (; miSprout != mapCryptedSproutSpendingKeys.end(); ++miSprout)
        {
            const libzcash::SproutPaymentAddress &address = (*miSprout).first;
            const std::vector<unsigned char> &vchCryptedSecret = (*miSprout).second;
            libzcash::SproutSpendingKey sk;
            if (!DecryptSproutSpendingKey(vMasterKeyIn, vchCryptedSecret, address, sk))
            {
                keyFail = true;
                break;
            }
            keyPass = true;
            if (fDecryptionThoroughlyChecked)
                break;
        }
        CryptedSaplingSpendingKeyMap::const_iterator miSapling = mapCryptedSaplingSpendingKeys.begin();
        for (; miSapling != mapCryptedSaplingSpendingKeys.end(); ++miSapling)
        {
            const libzcash::SaplingExtendedFullViewingKey &extfvk = (*miSapling).first;
            const std::vector<unsigned char> &vchCryptedSecret = (*miSapling).second;
            libzcash::SaplingExtendedSpendingKey sk;
            if (!DecryptSaplingSpendingKey(vMasterKeyIn, vchCryptedSecret, extfvk.fvk.GetFingerprint(), sk))
            {
                keyFail = true;
                break;
            }
            keyPass = true;
            if (fDecryptionThoroughlyChecked)
                break;
        }
        if (keyPass && keyFail)
        {
            LogPrintf("The wallet is probably corrupted: Some keys decrypt but not all.\n");
            assert(false);
        }
        if (keyFail || !keyPass)
            return false;
        vMasterKey = vMasterKeyIn;
        fDecryptionThoroughlyChecked = true;
    }
    NotifyStatusChanged(this);
    return true;
}

bool CCryptoKeyStore::SetHDSeed(const HDSeed& seed)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!IsCrypted()) {
            return CBasicKeyStore::SetHDSeed(seed);
        }

        if (IsLocked())
            return false;

        std::vector<unsigned char> vchCryptedSecret;
        // Use seed's fingerprint as IV
        // TODO: Handle this properly when we make encryption a supported feature
        auto seedFp = seed.Fingerprint();
        if (!EncryptSecret(vMasterKey, seed.RawSeed(), seedFp, vchCryptedSecret))
            return false;

        // This will call into CWallet to store the crypted seed to disk
        if (!SetCryptedHDSeed(seedFp, vchCryptedSecret))
            return false;
    }
    return true;
}

bool CCryptoKeyStore::SetCryptedHDSeed(
    const uint256& seedFp,
    const std::vector<unsigned char>& vchCryptedSecret)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!IsCrypted()) {
            return false;
        }

        if (!cryptedHDSeed.first.IsNull()) {
            // Don't allow an existing seed to be changed. We can maybe relax this
            // restriction later once we have worked out the UX implications.
            return false;
        }

        cryptedHDSeed = std::make_pair(seedFp, vchCryptedSecret);
    }
    return true;
}

bool CCryptoKeyStore::HaveHDSeed() const
{
    LOCK(cs_SpendingKeyStore);
    if (!IsCrypted())
        return CBasicKeyStore::HaveHDSeed();

    return !cryptedHDSeed.second.empty();
}

bool CCryptoKeyStore::GetHDSeed(HDSeed& seedOut) const
{
    LOCK(cs_SpendingKeyStore);
    if (!IsCrypted())
        return CBasicKeyStore::GetHDSeed(seedOut);

    if (cryptedHDSeed.second.empty())
        return false;

    return DecryptHDSeed(vMasterKey, cryptedHDSeed.second, cryptedHDSeed.first, seedOut);
}

bool CCryptoKeyStore::AddKeyPubKey(const CKey& key, const CPubKey &pubkey)
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::AddKeyPubKey(key, pubkey);

        if (IsLocked())
            return false;

        std::vector<unsigned char> vchCryptedSecret;
        CKeyingMaterial vchSecret(key.begin(), key.end());
        if (!EncryptSecret(vMasterKey, vchSecret, pubkey.GetHash(), vchCryptedSecret))
            return false;

        if (!AddCryptedKey(pubkey, vchCryptedSecret))
            return false;
    }
    return true;
}


bool CCryptoKeyStore::AddCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    {
        LOCK(cs_KeyStore);
        if (!SetCrypted())
            return false;

        mapCryptedKeys[vchPubKey.GetID()] = make_pair(vchPubKey, vchCryptedSecret);
    }
    return true;
}

bool CCryptoKeyStore::GetKey(const CKeyID &address, CKey& keyOut) const
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::GetKey(address, keyOut);

        CryptedKeyMap::const_iterator mi = mapCryptedKeys.find(address);
        if (mi != mapCryptedKeys.end())
        {
            const CPubKey &vchPubKey = (*mi).second.first;
            const std::vector<unsigned char> &vchCryptedSecret = (*mi).second.second;
            return DecryptKey(vMasterKey, vchCryptedSecret, vchPubKey, keyOut);
        }
    }
    return false;
}

bool CCryptoKeyStore::GetPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CKeyStore::GetPubKey(address, vchPubKeyOut);

        CryptedKeyMap::const_iterator mi = mapCryptedKeys.find(address);
        if (mi != mapCryptedKeys.end())
        {
            vchPubKeyOut = (*mi).second.first;
            return true;
        }
    }
    return false;
}

bool CCryptoKeyStore::AddSproutSpendingKey(const libzcash::SproutSpendingKey &sk)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::AddSproutSpendingKey(sk);

        if (IsLocked())
            return false;

        std::vector<unsigned char> vchCryptedSecret;
        CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << sk;
        CKeyingMaterial vchSecret(ss.begin(), ss.end());
        auto address = sk.address();
        if (!EncryptSecret(vMasterKey, vchSecret, address.GetHash(), vchCryptedSecret))
            return false;

        if (!AddCryptedSproutSpendingKey(address, sk.receiving_key(), vchCryptedSecret))
            return false;
    }
    return true;
}

bool CCryptoKeyStore::AddSaplingSpendingKey(
    const libzcash::SaplingExtendedSpendingKey &sk)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!IsCrypted()) {
            return CBasicKeyStore::AddSaplingSpendingKey(sk);
        }

        if (IsLocked()) {
            return false;
        }

        std::vector<unsigned char> vchCryptedSecret;
        CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << sk;
        CKeyingMaterial vchSecret(ss.begin(), ss.end());
        auto extfvk = sk.ToXFVK();
        if (!EncryptSecret(vMasterKey, vchSecret, extfvk.fvk.GetFingerprint(), vchCryptedSecret)) {
            return false;
        }

        if (!AddCryptedSaplingSpendingKey(extfvk, vchCryptedSecret, vMasterKey)) {
            return false;
        }
    }
    return true;
}

bool CCryptoKeyStore::AddSaplingExtendedFullViewingKey(
    const libzcash::SaplingExtendedFullViewingKey &extfvk)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!IsCrypted()) {
            return CBasicKeyStore::AddSaplingExtendedFullViewingKey(extfvk);
        }

        if (IsLocked()) {
            return false;
        }

        std::vector<unsigned char> vchCryptedSecret;
        CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << extfvk;
        CKeyingMaterial vchSecret(ss.begin(), ss.end());
        if (!EncryptSecret(vMasterKey, vchSecret, extfvk.fvk.GetFingerprint(), vchCryptedSecret)) {
            return false;
        }

        if (!AddCryptedSaplingExtendedFullViewingKey(extfvk, vchCryptedSecret)) {
            return false;
        }
    }
    return true;
}

bool CCryptoKeyStore::AddCryptedSproutSpendingKey(
    const libzcash::SproutPaymentAddress &address,
    const libzcash::ReceivingKey &rk,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!SetCrypted())
            return false;

        mapCryptedSproutSpendingKeys[address] = vchCryptedSecret;
        mapNoteDecryptors.insert(std::make_pair(address, ZCNoteDecryption(rk)));
    }
    return true;
}

bool CCryptoKeyStore::EncryptCScript(
    const uint256 &chash,
    const CScript &redeemScript,
    std::vector<unsigned char> &vchCryptedSecret)
{
    return EncryptCScript(chash, redeemScript, vchCryptedSecret, vMasterKey);
}

bool CCryptoKeyStore::EncryptCScript(
    const uint256 &chash,
    const CScript &redeemScript,
    std::vector<unsigned char> &vchCryptedSecret,
    CKeyingMaterial &vMasterKeyIn)
{
    CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    auto script = *(const CScriptBase*)(&redeemScript);
    ss << script;
    CKeyingMaterial vchSecret(ss.begin(), ss.end());
    if(!EncryptSecret(vMasterKeyIn, vchSecret, chash, vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::DecryptCScript(
    CScript &redeemScript,
    const uint256 &chash,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    LOCK(cs_SpendingKeyStore);
    if (!SetCrypted()) {
        return false;
    }

    if (IsLocked()) {
        return false;
    }

    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, chash, vchSecret)) {
        return false;
    }

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> *(CScriptBase*)(&redeemScript);

    return true;
}

bool CCryptoKeyStore::EncryptStringPair(
    const uint256 &chash,
    const std::string &stringIn1,
    const std::string &stringIn2,
    std::vector<unsigned char> &vchCryptedSecret)
{
    return EncryptStringPair(chash, stringIn1, stringIn2, vchCryptedSecret, vMasterKey);
}

bool CCryptoKeyStore::EncryptStringPair(
    const uint256 &chash,
    const std::string &stringIn1,
    const std::string &stringIn2,
    std::vector<unsigned char> &vchCryptedSecret,
    CKeyingMaterial &vMasterKeyIn)
{
    auto stringPair = make_pair(stringIn1, stringIn2);
    CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << stringPair;
    CKeyingMaterial vchSecret(ss.begin(), ss.end());
    if(!EncryptSecret(vMasterKeyIn, vchSecret, chash, vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::DecryptStringPair(
    std::string &stringOut1,
    std::string &stringOut2,
    const uint256 &chash,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    LOCK(cs_SpendingKeyStore);
    if (!SetCrypted()) {
        return false;
    }

    if (IsLocked()) {
        return false;
    }

    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, chash, vchSecret)) {
        return false;
    }

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> stringOut1;
    ss >> stringOut2;

    return true;
}

bool CCryptoKeyStore::EncryptPublicKey(
    const uint256 &chash,
    const CPubKey &vchPubKey,
    std::vector<unsigned char> &vchCryptedSecret)
{
    return EncryptPublicKey(chash, vchPubKey, vchCryptedSecret, vMasterKey);
}

bool CCryptoKeyStore::EncryptPublicKey(
    const uint256 &chash,
    const CPubKey &vchPubKey,
    std::vector<unsigned char> &vchCryptedSecret,
    CKeyingMaterial &vMasterKeyIn)
{
    CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << vchPubKey;
    CKeyingMaterial vchSecret(ss.begin(), ss.end());
    if(!EncryptSecret(vMasterKeyIn, vchSecret, chash, vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::DecryptPublicKey(
    CPubKey &vchPubKey,
    const uint256 &chash,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    LOCK(cs_SpendingKeyStore);
    if (!SetCrypted()) {
        return false;
    }

    if (IsLocked()) {
        return false;
    }

    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, chash, vchSecret)) {
        return false;
    }

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> vchPubKey;

    return true;
}

bool CCryptoKeyStore::DecryptWalletTransaction(
    const uint256 &chash,
    const std::vector<unsigned char> &vchCryptedSecret,
    CKeyingMaterial &vchSecret)
{
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, chash, vchSecret))
        return false;

    return true;
}

bool CCryptoKeyStore::EncryptWalletTransaction(
    const uint256 &hash,
    const CKeyingMaterial &vchSecret,
    std::vector<unsigned char> &vchCryptedSecret)
{
      return EncryptWalletTransaction(vMasterKey, hash, vchSecret, vchCryptedSecret);
}

bool CCryptoKeyStore::EncryptWalletTransaction(
    CKeyingMaterial &vMasterKeyIn,
    const uint256 &hash,
    const CKeyingMaterial &vchSecret,
    std::vector<unsigned char> &vchCryptedSecret)
{
    if(!EncryptSecret(vMasterKeyIn, vchSecret, hash, vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::EncryptSaplingMetaData(
    CKeyingMaterial &vMasterKeyIn,
    const CKeyMetadata &metadata,
    const libzcash::SaplingExtendedFullViewingKey &extfvk,
    std::vector<unsigned char> &vchCryptedSecret)
{
    CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << metadata;
    CKeyingMaterial vchSecret(ss.begin(), ss.end());
    if(!EncryptSecret(vMasterKeyIn, vchSecret, extfvk.fvk.GetFingerprint(), vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::DecryptSaplingMetaData(
     const std::vector<unsigned char>& vchCryptedSecret,
     const uint256 &extfvkFinger,
     CKeyMetadata &metadata)
{
    LOCK(cs_SpendingKeyStore);
    if (!IsCrypted()) {
        return false;
    }

    if (IsLocked()) {
        return false;
    }

    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, extfvkFinger, vchSecret)) {
        return false;
    }

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> metadata;
    return true;
}

bool CCryptoKeyStore::EncryptSaplingPrimarySpendingKey(
    const libzcash::SaplingExtendedSpendingKey &extsk,
    std::vector<unsigned char> &vchCryptedSecret)
{
    return EncryptSaplingPrimarySpendingKey(extsk, vchCryptedSecret, vMasterKey);
}

bool CCryptoKeyStore::EncryptSaplingPrimarySpendingKey(
    const libzcash::SaplingExtendedSpendingKey &extsk,
    std::vector<unsigned char> &vchCryptedSecret,
    CKeyingMaterial &vMasterKeyIn)
{
    CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << extsk;
    CKeyingMaterial vchSecret(ss.begin(), ss.end());
    if(!EncryptSecret(vMasterKeyIn, vchSecret, extsk.ToXFVK().fvk.GetFingerprint(), vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::DecryptSaplingPrimarySpendingKey(
    libzcash::SaplingExtendedSpendingKey &extsk,
    const uint256 &extfvkFinger,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    LOCK(cs_SpendingKeyStore);
    if (!SetCrypted()) {
        return false;
    }

    if (IsLocked()) {
        return false;
    }

    return DecryptSaplingSpendingKey(vMasterKey, vchCryptedSecret, extfvkFinger, extsk);

}

bool CCryptoKeyStore::EncryptSaplingPaymentAddress(
    const libzcash::SaplingIncomingViewingKey &ivk,
    const libzcash::SaplingPaymentAddress &addr,
    std::vector<unsigned char> &vchCryptedSecret)
{
    return EncryptSaplingPaymentAddress(ivk, addr, vchCryptedSecret, vMasterKey);
}

bool CCryptoKeyStore::EncryptSaplingPaymentAddress(
    const libzcash::SaplingIncomingViewingKey &ivk,
    const libzcash::SaplingPaymentAddress &addr,
    std::vector<unsigned char> &vchCryptedSecret,
    CKeyingMaterial &vMasterKeyIn)
{
    auto addressPair = std::make_pair(ivk, addr);
    CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << addressPair;
    CKeyingMaterial vchSecret(ss.begin(), ss.end());
    if(!EncryptSecret(vMasterKeyIn, vchSecret, addr.GetHash(), vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::DecryptSaplingPaymentAddress(
    libzcash::SaplingIncomingViewingKey &ivk,
    libzcash::SaplingPaymentAddress &addr,
    const uint256 &chash,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    LOCK(cs_SpendingKeyStore);
    if (!SetCrypted()) {
        return false;
    }

    if (IsLocked()) {
        return false;
    }

    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, chash, vchSecret)) {
        return false;
    }

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> ivk;
    ss >> addr;

    return true;
}

bool CCryptoKeyStore::EncryptSaplingDiversifiedAddress(
    const libzcash::SaplingPaymentAddress &addr,
    const libzcash::SaplingIncomingViewingKey &ivk,
    const blob88 &path,
    std::vector<unsigned char> &vchCryptedSecret)
{
    return EncryptSaplingDiversifiedAddress(addr, ivk, path, vchCryptedSecret, vMasterKey);
}

bool CCryptoKeyStore::EncryptSaplingDiversifiedAddress(
    const libzcash::SaplingPaymentAddress &addr,
    const libzcash::SaplingIncomingViewingKey &ivk,
    const blob88 &path,
    std::vector<unsigned char> &vchCryptedSecret,
    CKeyingMaterial &vMasterKeyIn)
{
    auto addressPair = std::make_pair(std::make_pair(addr, ivk),path);
    CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << addressPair;
    CKeyingMaterial vchSecret(ss.begin(), ss.end());
    if(!EncryptSecret(vMasterKeyIn, vchSecret, addr.GetHash(), vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::DecryptSaplingDiversifiedAddress(
    libzcash::SaplingPaymentAddress &addr,
    libzcash::SaplingIncomingViewingKey &ivk,
    blob88 &path,
    const uint256 &chash,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    LOCK(cs_SpendingKeyStore);
    if (!SetCrypted()) {
        return false;
    }

    if (IsLocked()) {
        return false;
    }

    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, chash, vchSecret)) {
        return false;
    }

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> addr;
    ss >> ivk;
    ss >> path;

    return true;
}

bool CCryptoKeyStore::EncryptSaplingLastDiversifierUsed(
    const uint256 &chash,
    const libzcash::SaplingIncomingViewingKey &ivk,
    const blob88 &path,
    std::vector<unsigned char> &vchCryptedSecret)
{
    return EncryptSaplingLastDiversifierUsed(chash, ivk, path, vchCryptedSecret, vMasterKey);
}

bool CCryptoKeyStore::EncryptSaplingLastDiversifierUsed(
    const uint256 &chash,
    const libzcash::SaplingIncomingViewingKey &ivk,
    const blob88 &path,
    std::vector<unsigned char> &vchCryptedSecret,
    CKeyingMaterial &vMasterKeyIn)
{
    auto addressPair = std::make_pair(ivk ,path);
    CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << addressPair;
    CKeyingMaterial vchSecret(ss.begin(), ss.end());
    if(!EncryptSecret(vMasterKeyIn, vchSecret, chash, vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool CCryptoKeyStore::DecryptSaplingLastDiversifierUsed(
    libzcash::SaplingIncomingViewingKey &ivk,
    blob88 &path,
    const uint256 &chash,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    LOCK(cs_SpendingKeyStore);
    if (!SetCrypted()) {
        return false;
    }

    if (IsLocked()) {
        return false;
    }

    CKeyingMaterial vchSecret;
    if (!DecryptSecret(vMasterKey, vchCryptedSecret, chash, vchSecret)) {
        return false;
    }

    CSecureDataStream ss(vchSecret, SER_NETWORK, PROTOCOL_VERSION);
    ss >> ivk;
    ss >> path;

    return true;
}

bool CCryptoKeyStore::AddCryptedSaplingSpendingKey(
    const libzcash::SaplingExtendedFullViewingKey &extfvk,
    const std::vector<unsigned char> &vchCryptedSecret,
    CKeyingMaterial &vMasterKeyIn)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!SetCrypted()) {
            return false;
        }

        // if SaplingFullViewingKey is not in SaplingFullViewingKeyMap, add it
        if (!CBasicKeyStore::AddSaplingExtendedFullViewingKey(extfvk)) {
            return false;
        }

        mapCryptedSaplingSpendingKeys[extfvk] = vchCryptedSecret;
    }
    return true;
}

bool CCryptoKeyStore::AddCryptedSaplingExtendedFullViewingKey(
    const libzcash::SaplingExtendedFullViewingKey &extfvk,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!IsCrypted()) {
            return false;
        }

        // if SaplingFullViewingKey is not in SaplingFullViewingKeyMap, add it
        return CBasicKeyStore::AddSaplingExtendedFullViewingKey(extfvk);

    }
    return true;
}

bool CCryptoKeyStore::AddCryptedSaplingPaymentAddress(
    const libzcash::SaplingIncomingViewingKey &ivk,
    const libzcash::SaplingPaymentAddress &addr,
    const std::vector<unsigned char> &vchCryptedSecret)
{

    LOCK(cs_SpendingKeyStore);
    if (!SetCrypted()) {
        return false;
    }

    return CBasicKeyStore::AddSaplingIncomingViewingKey(ivk, addr);

}

bool CCryptoKeyStore::AddCryptedSaplingDiversifiedAddress(
    const libzcash::SaplingIncomingViewingKey &ivk,
    const libzcash::SaplingPaymentAddress &addr,
    const blob88 &path,
    const std::vector<unsigned char> &vchCryptedSecret)
{

    LOCK(cs_SpendingKeyStore);
    if (!SetCrypted()) {
        return false;
    }

    return CBasicKeyStore::AddSaplingDiversifiedAddress(addr, ivk, path);

}

bool CCryptoKeyStore::LoadCryptedSaplingSpendingKey(
    const uint256 &extfvkFinger,
    const std::vector<unsigned char> &vchCryptedSecret,
    libzcash::SaplingExtendedFullViewingKey &extfvk)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!SetCrypted()) {
            return false;
        }

        if (IsLocked()) {
            return false;
        }

        libzcash::SaplingExtendedSpendingKey skOut;
        if (!DecryptSaplingSpendingKey(vMasterKey, vchCryptedSecret, extfvkFinger, skOut))
        {
            return false;
        }

        extfvk = skOut.ToXFVK();
        // if SaplingFullViewingKey is not in SaplingFullViewingKeyMap, add it
        if (!CBasicKeyStore::AddSaplingExtendedFullViewingKey(extfvk)) {
            return false;
        }

        mapCryptedSaplingSpendingKeys[extfvk] = vchCryptedSecret;
    }
    return true;
}

bool CCryptoKeyStore::LoadCryptedSaplingExtendedFullViewingKey(
    const uint256 &extfvkFinger,
    const std::vector<unsigned char> &vchCryptedSecret,
    libzcash::SaplingExtendedFullViewingKey &extfvk)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!SetCrypted()) {
            return false;
        }

        if (IsLocked()) {
            return false;
        }

        if (!DecryptSaplingExtendedFullViewingKey(vMasterKey, vchCryptedSecret, extfvkFinger, extfvk))
        {
            return false;
        }

        // if SaplingFullViewingKey is not in SaplingFullViewingKeyMap, add it
        if (!CBasicKeyStore::AddSaplingExtendedFullViewingKey(extfvk)) {
            return false;
        }

    }
    return true;
}

bool CCryptoKeyStore::LoadCryptedSaplingPaymentAddress(
    libzcash::SaplingIncomingViewingKey &ivk,
    libzcash::SaplingPaymentAddress &addr,
    const uint256 &chash,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!SetCrypted()) {
            return false;
        }

        if (IsLocked()) {
            return false;
        }

        if (!DecryptSaplingPaymentAddress(ivk, addr, chash, vchCryptedSecret))
        {
            return false;
        }

        // if SaplingFullViewingKey is not in SaplingFullViewingKeyMap, add it
        if (!CBasicKeyStore::AddSaplingIncomingViewingKey(ivk, addr)) {
            return false;
        }

    }
    return true;
}

bool CCryptoKeyStore::LoadCryptedSaplingDiversifiedAddress(
    const uint256 &chash,
    const std::vector<unsigned char> &vchCryptedSecret)
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!SetCrypted()) {
            return false;
        }

        if (IsLocked()) {
            return false;
        }

        libzcash::SaplingPaymentAddress addr;
        libzcash::SaplingIncomingViewingKey ivk;
        blob88 path;
        if (!DecryptSaplingDiversifiedAddress(addr, ivk, path, chash, vchCryptedSecret))
        {
            return false;
        }

        if (!CBasicKeyStore::AddSaplingDiversifiedAddress(addr, ivk, path)) {
            return false;
        }

    }
    return true;
}

bool CCryptoKeyStore::GetSproutSpendingKey(const libzcash::SproutPaymentAddress &address, libzcash::SproutSpendingKey &skOut) const
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::GetSproutSpendingKey(address, skOut);

        CryptedSproutSpendingKeyMap::const_iterator mi = mapCryptedSproutSpendingKeys.find(address);
        if (mi != mapCryptedSproutSpendingKeys.end())
        {
            const std::vector<unsigned char> &vchCryptedSecret = (*mi).second;
            return DecryptSproutSpendingKey(vMasterKey, vchCryptedSecret, address, skOut);
        }
    }
    return false;
}

bool CCryptoKeyStore::GetSaplingSpendingKey(const libzcash::SaplingExtendedFullViewingKey &extfvk, libzcash::SaplingExtendedSpendingKey &skOut) const
{
    {
        LOCK(cs_SpendingKeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::GetSaplingSpendingKey(extfvk, skOut);

        for (auto entry : mapCryptedSaplingSpendingKeys) {
            if (entry.first == extfvk) {
                const std::vector<unsigned char> &vchCryptedSecret = entry.second;
                return DecryptSaplingSpendingKey(vMasterKey, vchCryptedSecret, entry.first.fvk.GetFingerprint(), skOut);
            }
        }
    }
    return false;
}

bool CCryptoKeyStore::EncryptKeys(CKeyingMaterial& vMasterKeyIn)
{
    {
        LOCK2(cs_KeyStore, cs_SpendingKeyStore);
        if (!mapCryptedKeys.empty() || IsCrypted())
            return false;

        fUseCrypto = true;
        if (!hdSeed.IsNull()) {
            {
                std::vector<unsigned char> vchCryptedSecret;
                // Use seed's fingerprint as IV
                // TODO: Handle this properly when we make encryption a supported feature
                auto seedFp = hdSeed.Fingerprint();
                if (!EncryptSecret(vMasterKeyIn, hdSeed.RawSeed(), seedFp, vchCryptedSecret)) {
                    return false;
                }
                // This will call into CWallet to store the crypted seed to disk
                if (!SetCryptedHDSeed(seedFp, vchCryptedSecret)) {
                    return false;
                }
            }
            hdSeed = HDSeed();
        }
        BOOST_FOREACH(KeyMap::value_type& mKey, mapKeys)
        {
            const CKey &key = mKey.second;
            CPubKey vchPubKey = key.GetPubKey();
            CKeyingMaterial vchSecret(key.begin(), key.end());
            std::vector<unsigned char> vchCryptedSecret;
            if (!EncryptSecret(vMasterKeyIn, vchSecret, vchPubKey.GetHash(), vchCryptedSecret)) {
                return false;
            }
            if (!AddCryptedKey(vchPubKey, vchCryptedSecret)) {
                return false;
            }
        }
        mapKeys.clear();
        BOOST_FOREACH(SproutSpendingKeyMap::value_type& mSproutSpendingKey, mapSproutSpendingKeys)
        {
            const libzcash::SproutSpendingKey &sk = mSproutSpendingKey.second;
            CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss << sk;
            CKeyingMaterial vchSecret(ss.begin(), ss.end());
            libzcash::SproutPaymentAddress address = sk.address();
            std::vector<unsigned char> vchCryptedSecret;
            if (!EncryptSecret(vMasterKeyIn, vchSecret, address.GetHash(), vchCryptedSecret)) {
                return false;
            }
            if (!AddCryptedSproutSpendingKey(address, sk.receiving_key(), vchCryptedSecret)) {
                return false;
            }
        }
        mapSproutSpendingKeys.clear();
        //! Sapling key support
        BOOST_FOREACH(SaplingSpendingKeyMap::value_type& mSaplingSpendingKey, mapSaplingSpendingKeys)
        {
            const auto &sk = mSaplingSpendingKey.second;
            CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss << sk;
            CKeyingMaterial vchSecret(ss.begin(), ss.end());
            auto extfvk = sk.ToXFVK();
            std::vector<unsigned char> vchCryptedSecret;
            if (!EncryptSecret(vMasterKeyIn, vchSecret, extfvk.fvk.GetFingerprint(), vchCryptedSecret)) {
                return false;
            }
            if (!AddCryptedSaplingSpendingKey(extfvk, vchCryptedSecret, vMasterKeyIn)) {
                return false;
            }
        }

        //Encrypt Extended Full Viewing keys
        BOOST_FOREACH(SaplingFullViewingKeyMap::value_type& mSaplingFullViewingKey, mapSaplingFullViewingKeys)
        {
            const auto &extfvk = mSaplingFullViewingKey.second;
            if (!HaveSaplingSpendingKey(extfvk)) {

                CSecureDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                ss << extfvk;
                CKeyingMaterial vchSecret(ss.begin(), ss.end());
                std::vector<unsigned char> vchCryptedSecret;
                if (!EncryptSecret(vMasterKeyIn, vchSecret, extfvk.fvk.GetFingerprint(), vchCryptedSecret)) {
                    return false;
                }
                if (!AddCryptedSaplingExtendedFullViewingKey(extfvk, vchCryptedSecret)) {
                    return false;
                }
            }
        }

        //Encrypt SaplingPaymentAddress
        BOOST_FOREACH(SaplingIncomingViewingKeyMap::value_type& mSaplingIncomingViewingKeys, mapSaplingIncomingViewingKeys)
        {
            const auto &ivk = mSaplingIncomingViewingKeys.second;
            const auto &addr = mSaplingIncomingViewingKeys.first;

            std::vector<unsigned char> vchCryptedSecret;
            if (!EncryptSaplingPaymentAddress(ivk, addr, vchCryptedSecret, vMasterKeyIn)) {
                return false;
            }

            if (!AddCryptedSaplingPaymentAddress(ivk, addr, vchCryptedSecret)) {
                return false;
            }
        }

        //Encrypt SaplingPaymentAddress
        BOOST_FOREACH(SaplingPaymentAddresses::value_type& mSaplingPaymentAddresses, mapSaplingPaymentAddresses)
        {

            const auto &dPath = mSaplingPaymentAddresses.second;
            const auto &ivk = dPath.first;
            const auto &path = dPath.second;
            const auto &addr = mSaplingPaymentAddresses.first;

            std::vector<unsigned char> vchCryptedSecret;
            if (!EncryptSaplingDiversifiedAddress(addr, ivk, path, vchCryptedSecret, vMasterKeyIn)) {
               return false;
            }

            if (!AddCryptedSaplingDiversifiedAddress(ivk, addr, path, vchCryptedSecret)) {
                return false;
            }
        }

        mapSaplingSpendingKeys.clear();
    }
    return true;
}
