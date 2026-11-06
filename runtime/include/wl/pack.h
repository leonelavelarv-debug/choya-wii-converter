// Lector del paquete .wlpk mapeado en memoria (sin copias).
#pragma once
#include <cstdint>
#include <string>

#include "wl/format.h"

namespace wl {

#pragma pack(push, 1)
struct PackHeader {
    char magic[4];
    uint32_t version;
    uint32_t entry_count;
    uint32_t flags;
    uint64_t toc_offset;
    uint64_t names_offset;
    uint64_t names_size;
    uint64_t manifest_offset;
    uint64_t manifest_size;
    uint64_t reserved;
};

struct PackEntry {
    uint8_t kind;
    uint8_t format;
    uint8_t count;   // mips (textura) o canales (audio)
    uint8_t flags;
    uint32_t name_offset;
    uint64_t key;
    uint64_t data_offset;
    uint64_t data_size;
    uint16_t width;
    uint16_t height;
    uint32_t extra0;  // textura: formato GX original | audio: frecuencia
    uint32_t extra1;  // textura: crc32 del nivel 0  | audio: inicio de bucle (0xFFFFFFFF = sin bucle)
    uint32_t extra2;  // textura: tamaño del nivel 0 | audio: cuadros
};
#pragma pack(pop)

static_assert(sizeof(PackHeader) == 64, "cabecera de 64 bytes");
static_assert(sizeof(PackEntry) == 48, "entrada de 48 bytes");

constexpr uint32_t PACK_FLAG_GLES3 = 1;

class Pack {
public:
    Pack() = default;
    ~Pack();
    Pack(const Pack&) = delete;
    Pack& operator=(const Pack&) = delete;

    // Abre el paquete. Devuelve false y llena error() si falla.
    bool open(const std::string& path);
    void close();

    bool is_open() const { return base_ != nullptr; }
    const std::string& error() const { return error_; }
    uint32_t size() const { return header_ ? header_->entry_count : 0; }
    uint32_t flags() const { return header_ ? header_->flags : 0; }
    const PackEntry& entry(uint32_t i) const { return toc_[i]; }
    const char* name(const PackEntry& e) const;
    const uint8_t* data(const PackEntry& e) const { return base_ + e.data_offset; }
    std::string manifest() const;

    // Búsqueda binaria por clave. nullptr si no existe.
    const PackEntry* find(uint64_t key) const;
    const PackEntry* find_audio(const char* disc_path) const;

private:
    const uint8_t* base_ = nullptr;
    size_t length_ = 0;
    const PackHeader* header_ = nullptr;
    const PackEntry* toc_ = nullptr;
    std::string error_;
};

}  // namespace wl
