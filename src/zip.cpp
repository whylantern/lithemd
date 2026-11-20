#include "zip.h"
#include "common.h"

extern "C" {
#include "../vendor/miniz.h"
}

struct ZipReader::Impl {
    std::vector<uint8_t> bytes;
    mz_zip_archive zip{};
    bool open = false;
    ~Impl() {
        if (open) mz_zip_reader_end(&zip);
    }
};

ZipReader::ZipReader() : impl_(new Impl()) {}
ZipReader::~ZipReader() = default;

bool ZipReader::Open(std::vector<uint8_t> bytes, std::wstring* error) {
    if (impl_->open) {
        mz_zip_reader_end(&impl_->zip);
        impl_->open = false;
    }
    impl_->bytes = std::move(bytes);
    memset(&impl_->zip, 0, sizeof(impl_->zip));
    if (!mz_zip_reader_init_mem(&impl_->zip, impl_->bytes.data(), impl_->bytes.size(), 0)) {
        if (error) *error = L"不是有效的 ZIP/OOXML 文件";
        return false;
    }
    impl_->open = true;
    return true;
}

bool ZipReader::Has(const std::wstring& name) const {
    if (!impl_->open) return false;
    std::string u = Utf8FromWide(name);
    return mz_zip_reader_locate_file(&impl_->zip, u.c_str(), nullptr, 0) >= 0;
}

bool ZipReader::Read(const std::wstring& name, std::vector<uint8_t>& out) const {
    out.clear();
    if (!impl_->open) return false;
    std::string u = Utf8FromWide(name);
    int idx = mz_zip_reader_locate_file(&impl_->zip, u.c_str(), nullptr, 0);
    if (idx < 0) {
        // Try a case-insensitive scan, as some producers vary case.
        std::wstring want = ToLower(name);
        mz_uint n = mz_zip_reader_get_num_files(&impl_->zip);
        for (mz_uint i = 0; i < n; i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&impl_->zip, i, &st)) continue;
            if (ToLower(WideFromUtf8(st.m_filename)) == want) { idx = (int)i; break; }
        }
    }
    if (idx < 0) return false;
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&impl_->zip, (mz_uint)idx, &st)) return false;
    // Check the declared uncompressed size before decompressing. Otherwise a
    // malicious ZIP could allocate an unbounded buffer first and only then hit
    // the post-extraction size check.
    if (st.m_uncomp_size > 256ull * 1024 * 1024) return false;
    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&impl_->zip, (mz_uint)idx, &sz, 0);
    if (!p) return false;
    if (sz > 256ull * 1024 * 1024) { mz_free(p); return false; }
    out.assign((uint8_t*)p, (uint8_t*)p + sz);
    mz_free(p);
    return true;
}

bool ZipReader::ReadText(const std::wstring& name, std::wstring& out, std::wstring* error) const {
    std::vector<uint8_t> data;
    if (!Read(name, data)) {
        if (error) *error = L"压缩包中缺少 " + name;
        return false;
    }
    std::wstring enc;
    out = DecodeBytes(data, enc);
    // XML in OOXML is always UTF-8, but honour a UTF-16 BOM if present.
    return true;
}

std::vector<std::wstring> ZipReader::Names() const {
    std::vector<std::wstring> out;
    if (!impl_->open) return out;
    mz_uint n = mz_zip_reader_get_num_files(&impl_->zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&impl_->zip, i, &st))
            out.push_back(WideFromUtf8(st.m_filename));
    }
    return out;
}
