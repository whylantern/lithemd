#include "archive_reader.h"
#include "common.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <cwctype>

extern "C" {
#include "../vendor/miniz.h"
}

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kZip64EocdSig = 0x06064b50;
constexpr uint32_t kZip64LocatorSig = 0x07064b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;

uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t Rd64(const uint8_t* p) { return (uint64_t)Rd32(p) | ((uint64_t)Rd32(p + 4) << 32); }

bool Bounds(size_t off, size_t len, size_t total) {
    return off <= total && len <= total - off;
}

std::wstring DecodeName(const uint8_t* data, size_t len, bool utf8Flag) {
    if (len == 0) return L"";
    std::string raw((const char*)data, len);
    if (utf8Flag) {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0);
        if (n > 0) {
            std::wstring out((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &out[0], n);
            return out;
        }
    } else {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0);
        if (n > 0) {
            std::wstring out((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &out[0], n);
            return out;
        }
    }
    int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), &out[0], n);
    return out;
}

// ---------------------------------------------------------------- checksums
const uint32_t* CrcTablePtr() {
    static const std::vector<uint32_t> table = [] {
        std::vector<uint32_t> values(256);
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            values[i] = c;
        }
        return values;
    }();
    return table.data();
}

uint32_t CrcUpdate(uint32_t crc, const uint8_t* data, size_t len) {
    const uint32_t* table = CrcTablePtr();
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t CrcByte(uint32_t crc, uint8_t b) {
    const uint32_t* table = CrcTablePtr();
    return table[(crc ^ b) & 0xFF] ^ (crc >> 8);
}

// ------------------------------------------------------------ ZipCrypto
struct ZipCrypto {
    uint32_t k0 = 0x12345678, k1 = 0x23456789, k2 = 0x34567890;

    void Update(uint8_t c) {
        k0 = CrcByte(k0, c);
        k1 = (k1 + (k0 & 0xFF)) * 134775813u + 1;
        k2 = CrcByte(k2, (uint8_t)(k1 >> 24));
    }
    uint8_t DecryptByte() const {
        uint32_t temp = k2 | 2;
        return (uint8_t)((temp * (temp ^ 1)) >> 8);
    }
    void Decrypt(uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            uint8_t plain = (uint8_t)(data[i] ^ DecryptByte());
            data[i] = plain;
            Update(plain);
        }
    }
};

// --------------------------------------------------------------- AES/HMAC
class BCryptHash {
public:
    BCryptHash() = default;
    ~BCryptHash() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    bool Begin(const std::vector<uint8_t>& key) {
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA1_ALGORITHM, nullptr,
                                                    BCRYPT_ALG_HANDLE_HMAC_FLAG)))
            return false;
        if (!NT_SUCCESS(BCryptCreateHash(alg_, &hash_, nullptr, 0,
                                         key.empty() ? nullptr : (PUCHAR)key.data(),
                                         (ULONG)key.size(), 0)))
            return false;
        return true;
    }
    bool Update(const uint8_t* data, size_t len) {
        return hash_ && NT_SUCCESS(BCryptHashData(hash_, (PUCHAR)data, (ULONG)len, 0));
    }
    bool Finish(uint8_t* out, size_t outLen) {
        return hash_ && NT_SUCCESS(BCryptFinishHash(hash_, out, (ULONG)outLen, 0));
    }
private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

bool AesCtrDecrypt(const std::vector<uint8_t>& key, const uint8_t* input, size_t len,
                   std::vector<uint8_t>& out) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    bool ok = false;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0))) return false;
    if (!NT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                      sizeof(BCRYPT_CHAIN_MODE_ECB), 0)))
        goto done;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(alg, &keyHandle, nullptr, 0,
                                               (PUCHAR)key.data(), (ULONG)key.size(), 0)))
        goto done;
    {
        out.resize(len);
        const size_t blocksPerChunk = 1024;
        std::vector<uint8_t> counters(blocksPerChunk * 16);
        std::vector<uint8_t> stream(blocksPerChunk * 16);
        size_t doneBytes = 0;
        uint64_t counter = 1;
        while (doneBytes < len) {
            size_t chunkBlocks = std::min(blocksPerChunk, (len - doneBytes + 15) / 16);
            for (size_t i = 0; i < chunkBlocks; i++) {
                uint8_t* c = &counters[i * 16];
                memset(c, 0, 16);
                uint64_t value = counter + i;
                for (int b = 0; b < 8; b++) c[b] = (uint8_t)(value >> (8 * b));
            }
            ULONG produced = 0;
            if (!NT_SUCCESS(BCryptEncrypt(keyHandle, counters.data(), (ULONG)(chunkBlocks * 16),
                                          nullptr, nullptr, 0, stream.data(), (ULONG)(chunkBlocks * 16),
                                          &produced, 0)))
                goto done;
            size_t chunkBytes = std::min<size_t>(len - doneBytes, chunkBlocks * 16);
            for (size_t i = 0; i < chunkBytes; i++)
                out[doneBytes + i] = (uint8_t)(input[doneBytes + i] ^ stream[i]);
            doneBytes += chunkBytes;
            counter += chunkBlocks;
        }
        ok = true;
    }
done:
    if (keyHandle) BCryptDestroyKey(keyHandle);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool DeriveAesKeys(const std::wstring& password, const uint8_t* salt, size_t saltLen,
                   size_t keyLen, std::vector<uint8_t>& material) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    bool ok = false;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr,
                                                BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return false;
    std::string pwd = Utf8FromWide(password);
    material.assign(keyLen * 2 + 2, 0);
    NTSTATUS status = BCryptDeriveKeyPBKDF2(alg, (PUCHAR)pwd.data(), (ULONG)pwd.size(),
                                            (PUCHAR)salt, (ULONG)saltLen, 1000,
                                            material.data(), (ULONG)material.size(), 0);
    ok = NT_SUCCESS(status);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool HmacSha1(const std::vector<uint8_t>& key, const uint8_t* data, size_t len,
              uint8_t out[20]) {
    BCryptHash hash;
    if (!hash.Begin(key)) return false;
    if (!hash.Update(data, len)) return false;
    return hash.Finish(out, 20);
}

bool ConstantEqual(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

// ------------------------------------------------------------------ paths
std::wstring RelativeEntryPath(const std::wstring& name) {
    std::wstring s = name;
    for (auto& c : s) if (c == L'/') c = L'\\';
    return s;
}

bool IsReservedName(const std::wstring& component) {
    std::wstring base = component;
    size_t dot = base.find(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    base = ToLower(base);
    static const wchar_t* names[] = {
        L"con", L"prn", L"aux", L"nul", L"com1", L"com2", L"com3", L"com4", L"com5",
        L"com6", L"com7", L"com8", L"com9", L"lpt1", L"lpt2", L"lpt3", L"lpt4", L"lpt5",
        L"lpt6", L"lpt7", L"lpt8", L"lpt9"
    };
    for (auto* n : names)
        if (base == n) return true;
    return false;
}

bool SafeEntryName(const std::wstring& name) {
    if (name.empty()) return false;
    std::wstring s = name;
    for (auto& c : s) if (c == L'/') c = L'\\';
    if (s.size() > 1024) return false;
    if (s.front() == L'\\' || s.front() == L'/') return false;
    if (s.size() >= 2 && s[1] == L':') return false;
    if (s.find(L':') != std::wstring::npos) return false;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(L'\\', start);
        std::wstring part = s.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part == L".." || part == L".") return false;
        if (!part.empty()) {
            if (part.back() == L' ' || part.back() == L'.') return false;
            if (IsReservedName(part)) return false;
            for (wchar_t c : part) if (c < 32) return false;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}

} // namespace

bool ArchiveReader::IsSafeEntryName(const std::wstring& name) {
    return SafeEntryName(name);
}

void ArchiveReader::Close() {
    bytes_.clear();
    entries_.clear();
    extras_.clear();
    path_.clear();
}

bool ArchiveReader::Open(const std::wstring& path, std::wstring* error) {
    std::wstring full = AbsolutePath(path);
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(full, bytes, error)) {
        if (error && error->empty()) *error = L"无法读取压缩包";
        return false;
    }
    if (!OpenBytes(std::move(bytes), error)) return false;
    path_ = full;
    return true;
}

bool ArchiveReader::OpenBytes(std::vector<uint8_t> bytes, std::wstring* error) {
    Close();
    bytes_ = std::move(bytes);
    auto fail = [&](const std::wstring& message) {
        if (error) *error = message;
        Close();
        return false;
    };
    if (bytes_.size() < 22) return fail(L"不是有效的 ZIP 文件");
    size_t minEocd = bytes_.size() > 65557 ? bytes_.size() - 65557 : 0;
    size_t eocd = SIZE_MAX;
    for (size_t i = bytes_.size() - 22 + 1; i-- > minEocd;) {
        if (Rd32(&bytes_[i]) == kEocdSig) { eocd = i; break; }
    }
    if (eocd == SIZE_MAX) return fail(L"不是有效的 ZIP 文件");
    if (!Bounds(eocd, 22, bytes_.size())) return fail(L"ZIP 目录损坏");
    uint16_t disk = Rd16(&bytes_[eocd + 4]);
    uint16_t cdDisk = Rd16(&bytes_[eocd + 6]);
    uint64_t count = Rd16(&bytes_[eocd + 10]);
    uint64_t cdSize = Rd32(&bytes_[eocd + 12]);
    uint64_t cdOffset = Rd32(&bytes_[eocd + 16]);
    if (disk != 0 || cdDisk != 0) return fail(L"不支持分卷 ZIP");
    if (count == 0xFFFF || cdSize == 0xFFFFFFFF || cdOffset == 0xFFFFFFFF) {
        if (eocd < 20 || Rd32(&bytes_[eocd - 20]) != kZip64LocatorSig)
            return fail(L"ZIP64 目录损坏");
        size_t z64 = (size_t)Rd64(&bytes_[eocd - 20 + 8]);
        if (!Bounds(z64, 56, bytes_.size()) || Rd32(&bytes_[z64]) != kZip64EocdSig)
            return fail(L"ZIP64 目录损坏");
        count = Rd64(&bytes_[z64 + 32]);
        cdSize = Rd64(&bytes_[z64 + 40]);
        cdOffset = Rd64(&bytes_[z64 + 48]);
    }
    if (count > 100000) return fail(L"压缩包条目过多");
    if (!Bounds((size_t)cdOffset, (size_t)cdSize, bytes_.size())) return fail(L"ZIP 目录损坏");
    size_t p = (size_t)cdOffset;
    size_t cdEnd = p + (size_t)cdSize;
    entries_.reserve((size_t)count);
    extras_.reserve((size_t)count);
    for (uint64_t i = 0; i < count; i++) {
        if (!Bounds(p, 46, cdEnd) || Rd32(&bytes_[p]) != kCentralSig)
            return fail(L"ZIP 目录损坏");
        ArchiveEntry entry;
        entry.flags = Rd16(&bytes_[p + 8]);
        entry.method = Rd16(&bytes_[p + 10]);
        entry.dosTime = ((uint64_t)Rd16(&bytes_[p + 14]) << 16) | Rd16(&bytes_[p + 12]);
        entry.crc = Rd32(&bytes_[p + 16]);
        uint64_t compressed = Rd32(&bytes_[p + 20]);
        uint64_t size = Rd32(&bytes_[p + 24]);
        uint16_t nameLen = Rd16(&bytes_[p + 28]);
        uint16_t extraLen = Rd16(&bytes_[p + 30]);
        uint16_t commentLen = Rd16(&bytes_[p + 32]);
        uint32_t externalAttr = Rd32(&bytes_[p + 38]);
        uint64_t localOffset = Rd32(&bytes_[p + 42]);
        if (!Bounds(p + 46, (size_t)nameLen + extraLen + commentLen, cdEnd))
            return fail(L"ZIP 目录损坏");
        entry.name = DecodeName(&bytes_[p + 46], nameLen, (entry.flags & 0x800) != 0);
        EntryExtra extra;
        size_t ep = p + 46 + nameLen;
        size_t ee = ep + extraLen;
        while (ep + 4 <= ee) {
            uint16_t id = Rd16(&bytes_[ep]);
            uint16_t len = Rd16(&bytes_[ep + 2]);
            if (ep + 4 + len > ee) break;
            const uint8_t* data = &bytes_[ep + 4];
            if (id == 0x0001) {
                size_t q = 0;
                if (size == 0xFFFFFFFF && q + 8 <= len) { size = Rd64(data + q); q += 8; }
                if (compressed == 0xFFFFFFFF && q + 8 <= len) { compressed = Rd64(data + q); q += 8; }
                if (localOffset == 0xFFFFFFFF && q + 8 <= len) { localOffset = Rd64(data + q); q += 8; }
            } else if (id == 0x9901 && len >= 7) {
                extra.hasAes = true;
                uint8_t strength = data[4];
                extra.aesStrength = strength;
                extra.aesMethod = Rd16(data + 5);
            }
            ep += 4 + len;
        }
        entry.encrypted = (entry.flags & 1) != 0;
        entry.aes = extra.hasAes;
        if (extra.hasAes) entry.method = extra.aesMethod;
        entry.size = size;
        entry.compressed = compressed;
        entry.localOffset = localOffset;
        entry.index = (uint32_t)i;
        entry.isDir = (!entry.name.empty() && (entry.name.back() == L'/' || entry.name.back() == L'\\')) ||
                      (externalAttr & 0x10) != 0;
        entries_.push_back(std::move(entry));
        extras_.push_back(extra);
        p += 46 + (size_t)nameLen + extraLen + commentLen;
    }
    if (entries_.size() > 100000) return fail(L"压缩包条目过多");
    return true;
}

bool ArchiveReader::HasEncrypted() const {
    for (auto& e : entries_) if (e.encrypted && !e.isDir) return true;
    return false;
}

uint64_t ArchiveReader::TotalSize() const {
    uint64_t total = 0;
    for (auto& e : entries_) {
        if (total > UINT64_MAX - e.size) return UINT64_MAX;
        total += e.size;
    }
    return total;
}

bool ArchiveReader::Read(size_t index, const std::wstring& password, std::vector<uint8_t>& out,
                         std::wstring* error) const {
    out.clear();
    auto fail = [&](const std::wstring& message) {
        if (error) *error = message;
        return false;
    };
    if (index >= entries_.size()) return fail(L"压缩包条目不存在");
    const ArchiveEntry& entry = entries_[index];
    if (entry.isDir) return fail(L"这是文件夹条目");
    if (entry.localOffset >= bytes_.size() || !Bounds((size_t)entry.localOffset, 30, bytes_.size()))
        return fail(L"ZIP 文件头损坏");
    size_t local = (size_t)entry.localOffset;
    if (Rd32(&bytes_[local]) != kLocalSig) return fail(L"ZIP 文件头损坏");
    uint16_t nameLen = Rd16(&bytes_[local + 26]);
    uint16_t extraLen = Rd16(&bytes_[local + 28]);
    size_t dataOff = local + 30 + (size_t)nameLen + extraLen;
    if (dataOff > bytes_.size() || entry.compressed > bytes_.size() - dataOff)
        return fail(L"ZIP 数据超出文件范围");
    const uint8_t* payload = &bytes_[dataOff];
    size_t payloadLen = (size_t)entry.compressed;
    if (entry.encrypted && password.empty()) return fail(L"该条目需要密码");
    if (entry.size > ArchiveLimits().maxEntrySize) return fail(L"条目过大，已停止解压");
    if (entry.compressed > 0 && entry.size / entry.compressed > ArchiveLimits().maxRatio &&
        entry.size > 16ull * 1024 * 1024)
        return fail(L"压缩比异常，已拒绝解压");

    std::vector<uint8_t> decrypted;
    if (entry.encrypted) {
        if (entry.aes) {
            const EntryExtra& extra = extras_[index];
            size_t saltLen = extra.aesStrength == 1 ? 8 : (extra.aesStrength == 2 ? 12 : 16);
            size_t keyLen = extra.aesStrength == 1 ? 16 : (extra.aesStrength == 2 ? 24 : 32);
            if (extra.aesStrength < 1 || extra.aesStrength > 3)
                return fail(L"不支持的 AES 强度");
            if (payloadLen < saltLen + 12) return fail(L"AES 数据损坏");
            size_t encryptedLen = payloadLen - saltLen - 2 - 10;
            std::vector<uint8_t> material;
            if (!DeriveAesKeys(password, payload, saltLen, keyLen, material))
                return fail(L"无法派生 AES 密钥");
            if (!ConstantEqual(payload + saltLen, material.data() + keyLen * 2, 2))
                return fail(L"密码错误");
            std::vector<uint8_t> key(material.begin(), material.begin() + keyLen);
            std::vector<uint8_t> macKey(material.begin() + keyLen, material.begin() + keyLen * 2);
            std::vector<uint8_t> plain;
            const uint8_t* cipher = payload + saltLen + 2;
            if (!AesCtrDecrypt(key, cipher, encryptedLen, plain))
                return fail(L"AES 解密失败");
            uint8_t digest[20]{};
            if (!HmacSha1(macKey, cipher, encryptedLen, digest))
                return fail(L"AES 校验失败");
            if (!ConstantEqual(digest, payload + payloadLen - 10, 10))
                return fail(L"密码错误或文件校验失败");
            decrypted = std::move(plain);
            payload = decrypted.data();
            payloadLen = decrypted.size();
        } else {
            if (payloadLen < 12) return fail(L"ZIP 加密数据损坏");
            uint8_t check = (uint8_t)(entry.crc >> 24);
            if ((entry.flags & 8) != 0) {
                // When a data descriptor is present some writers use the high
                // byte of the DOS time as the check byte.
                check = (uint8_t)(entry.dosTime >> 8);
            }
            std::vector<std::string> candidates;
            candidates.push_back(Utf8FromWide(password));
            // Older Windows ZIP tools use the ANSI code page for passwords.
            if (password.size() > 0) {
                int need = WideCharToMultiByte(CP_ACP, 0, password.data(), (int)password.size(),
                                               nullptr, 0, nullptr, nullptr);
                if (need > 0) {
                    std::string acp((size_t)need, '\0');
                    WideCharToMultiByte(CP_ACP, 0, password.data(), (int)password.size(),
                                        &acp[0], need, nullptr, nullptr);
                    if (acp != candidates.front()) candidates.push_back(std::move(acp));
                }
            }
            bool unlocked = false;
            for (auto& pwd : candidates) {
                std::vector<uint8_t> plain(payload, payload + payloadLen);
                ZipCrypto crypto;
                for (unsigned char c : pwd) crypto.Update(c);
                crypto.Decrypt(plain.data(), plain.size());
                if (plain[11] != check) continue;
                decrypted.assign(plain.begin() + 12, plain.end());
                unlocked = true;
                break;
            }
            if (!unlocked) return fail(L"密码错误");
            payload = decrypted.data();
            payloadLen = decrypted.size();
        }
    }

    if (entry.method == 0) {
        if (payloadLen > ArchiveLimits().maxEntrySize) return fail(L"条目过大");
        out.assign(payload, payload + payloadLen);
    } else if (entry.method == 8) {
        size_t outLen = 0;
        void* data = tinfl_decompress_mem_to_heap(payload, payloadLen, &outLen, 0);
        if (!data) return fail(L"解压失败，密码可能不正确");
        if (outLen > ArchiveLimits().maxEntrySize) {
            mz_free(data);
            return fail(L"条目过大，已停止解压");
        }
        out.assign((uint8_t*)data, (uint8_t*)data + outLen);
        mz_free(data);
    } else {
        return fail(L"不支持的 ZIP 压缩方式");
    }
    if (entry.size != 0 && out.size() != entry.size)
        return fail(L"解压大小与目录记录不一致");
    if (entry.crc != 0 && CrcUpdate(0, out.data(), out.size()) != entry.crc)
        return fail(L"CRC 校验失败，密码可能不正确");
    return true;
}

bool ArchiveReader::ExtractAll(const std::wstring& destDir, const std::wstring& password,
                               const std::function<bool(size_t, const std::wstring&)>& progress,
                               bool overwrite, std::wstring* error) const {
    auto fail = [&](const std::wstring& message) {
        if (error) *error = message;
        return false;
    };
    if (!IsOpen()) return fail(L"压缩包尚未打开");
    std::wstring dest = AbsolutePath(destDir);
    if (!EnsureDir(dest)) return fail(L"无法创建目标文件夹");
    if (entries_.size() > ArchiveLimits().maxEntries) return fail(L"压缩包条目过多");
    uint64_t total = 0;
    for (auto& entry : entries_) {
        if (entry.isDir) continue;
        if (!SafeEntryName(entry.name))
            return fail(L"压缩包包含不安全的路径：" + entry.name);
        if (entry.size > ArchiveLimits().maxEntrySize) return fail(L"条目过大：" + entry.name);
        if (entry.compressed > 0 && entry.size / entry.compressed > ArchiveLimits().maxRatio &&
            entry.size > 16ull * 1024 * 1024)
            return fail(L"压缩比异常：" + entry.name);
        if (total > ArchiveLimits().maxTotalSize - entry.size) return fail(L"解压总大小超过限制");
        total += entry.size;
        std::wstring target = PathJoin(dest, RelativeEntryPath(entry.name));
        if (!overwrite && FileExists(target)) return fail(L"目标文件已存在：" + entry.name);
    }
    for (size_t i = 0; i < entries_.size(); i++) {
        const ArchiveEntry& entry = entries_[i];
        if (progress && !progress(i, entry.name)) return fail(L"已取消解压");
        std::wstring rel = RelativeEntryPath(entry.name);
        std::wstring target = PathJoin(dest, rel);
        if (entry.isDir) {
            if (!EnsureDir(target)) return fail(L"无法创建文件夹：" + entry.name);
            continue;
        }
        if (!SafeEntryName(entry.name)) return fail(L"不安全的路径：" + entry.name);
        std::wstring full = AbsolutePath(target);
        std::wstring lowerDest = ToLower(dest);
        std::wstring lowerFull = ToLower(full);
        if (lowerFull.size() <= lowerDest.size() + 1 ||
            lowerFull.compare(0, lowerDest.size(), lowerDest) != 0 ||
            (full[dest.size()] != L'\\' && full[dest.size()] != L'/'))
            return fail(L"不安全的路径：" + entry.name);
        if (!EnsureDir(PathDir(full))) return fail(L"无法创建文件夹：" + entry.name);
        std::vector<uint8_t> data;
        std::wstring readError;
        if (!Read(i, password, data, &readError))
            return fail(readError.empty() ? L"解压失败：" + entry.name : readError);
        if (!overwrite && FileExists(full)) return fail(L"目标文件已存在：" + entry.name);
        std::wstring part = full + L".lithemd-part";
        if (!WriteFileBytes(part, data, &readError)) return fail(L"写入失败：" + entry.name);
        if (!MoveFileExW(part.c_str(), full.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(part.c_str());
            return fail(L"无法写入文件：" + entry.name);
        }
    }
    return true;
}
