#include "wl/pack.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace wl {

Pack::~Pack() { close(); }

void Pack::close() {
    if (base_) munmap(const_cast<uint8_t*>(base_), length_);
    base_ = nullptr;
    header_ = nullptr;
    toc_ = nullptr;
    length_ = 0;
}

bool Pack::open(const std::string& path) {
    close();
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error_ = "no se pudo abrir " + path;
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(PackHeader))) {
        ::close(fd);
        error_ = "archivo demasiado pequeño";
        return false;
    }
    void* m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        error_ = "mmap falló";
        return false;
    }
    base_ = static_cast<const uint8_t*>(m);
    length_ = size_t(st.st_size);
    header_ = reinterpret_cast<const PackHeader*>(base_);
    if (std::memcmp(header_->magic, "WLPK", 4) != 0 || header_->version != 1) {
        error_ = "no es un paquete .wlpk versión 1";
        close();
        return false;
    }
    const uint64_t toc_end = header_->toc_offset + uint64_t(header_->entry_count) * sizeof(PackEntry);
    if (toc_end > length_ || header_->names_offset + header_->names_size > length_) {
        error_ = "tabla de contenido fuera del archivo";
        close();
        return false;
    }
    toc_ = reinterpret_cast<const PackEntry*>(base_ + header_->toc_offset);
    for (uint32_t i = 0; i < header_->entry_count; ++i) {
        const PackEntry& e = toc_[i];
        if (e.data_offset + e.data_size > length_ || e.name_offset >= header_->names_size) {
            error_ = "entrada dañada";
            close();
            return false;
        }
    }
    error_.clear();
    return true;
}

const char* Pack::name(const PackEntry& e) const {
    return reinterpret_cast<const char*>(base_ + header_->names_offset + e.name_offset);
}

std::string Pack::manifest() const {
    if (!header_) return {};
    return std::string(reinterpret_cast<const char*>(base_ + header_->manifest_offset),
                       size_t(header_->manifest_size));
}

const PackEntry* Pack::find(uint64_t key) const {
    if (!toc_) return nullptr;
    uint32_t lo = 0, hi = header_->entry_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (toc_[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return (lo < header_->entry_count && toc_[lo].key == key) ? &toc_[lo] : nullptr;
}

const PackEntry* Pack::find_audio(const char* disc_path) const {
    const PackEntry* e = find(path_key(disc_path));
    return (e && e->kind == uint8_t(Kind::Audio)) ? e : nullptr;
}

}  // namespace wl
