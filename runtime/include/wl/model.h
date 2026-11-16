// Mallas del paquete (entrada KIND_MODEL, bloque "CMDL"; ver choya/model_pack.py).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "wl/pack.h"

namespace wl {

constexpr uint8_t KIND_MODEL = 3;
constexpr uint8_t KIND_ANIM = 4;  // animación de huesos horneada (bloque "CANM"; ver choya/anim.py)
constexpr uint8_t KIND_MATANIM = 5;  // SRT0/CLR0/PAT0/VIS0 horneada para un modelo (bloque "CMAN"; choya/anim_mat.py)
constexpr uint8_t KIND_SCENE = 6;    // escena SCN0 horneada (bloque "CSCN"; choya/scn0.py)

#pragma pack(push, 1)
struct ModelVertexV2 {  // versiones 1 y 2: sin huesos
    float pos[3];
    float nrm[3];
    uint8_t col[4];
    float uv[2];
};
struct ModelVertex {    // versión 3
    float pos[3];
    float nrm[3];
    uint8_t col[4];
    float uv[2];
    uint8_t bone[4];    // índice en ModelView::bones
    uint8_t weight[4];  // suman 255; los cuatro en 0 = vértice fijo
};
struct ModelBone {
    int32_t parent;     // -1 = raíz
    uint32_t flags;
    float inv_rest[12]; // inversa de la pose de reposo, 3x4 por filas
    uint32_t name_hash; // FNV-1a de 32 bits del nombre
    uint32_t reserved;
};
struct ModelSubmesh {
    uint32_t first;
    uint32_t count;
    uint64_t texture_key;  // 0 = sin textura
    uint8_t cull;          // 0 ninguno, 1 frente, 2 atrás, 3 todo
    uint8_t xlu;           // translúcido: se dibuja al final con mezcla
    uint8_t wrap_s;        // 0 clamp, 1 repetir, 2 espejo
    uint8_t wrap_t;
    uint32_t material;     // índice en ModelView::materials (versión 2)
    uint8_t vis_flags;     // versión 4: bit0 = oculta por defecto (su nodo no tiene FLAG_VISIBLE)
    uint8_t pad;
    uint16_t vis_node;     // versión 4: nodo de visibilidad (0xFFFF = ninguno)
    uint32_t reserved;
};

// Material TEV (512 bytes; ver choya/model_pack.py). Registros en los mismos bits que el hardware GX.
struct TevStageRec {
    uint32_t color_env;
    uint32_t alpha_env;
    uint8_t texmap, texcoord, tex_on, ras;
};
struct TevMapRec {
    uint64_t texture_key;
    uint8_t wrap_s, wrap_t, present, pad[5];
};
// Canal de luz del material (ResChanData de NW4R): colores y el registro de control de GX (XF 0x100E).
struct ChanRec {
    uint32_t flags;       // 1 color de material, 2 alfa de material, 4 color ambiente, 8 alfa ambiente,
                          // 16 control del color, 32 control del alfa (lo que trae el material)
    uint8_t mat[4];
    uint8_t amb[4];
    uint32_t ctrl_color;  // bit 0 material del vértice, 1 luz encendida, 2-5 y 11-14 luces, 6 ambiente del vértice,
    uint32_t ctrl_alpha;  // 7-8 función difusa (0 ninguna, 1 con signo, 2 recortada), 9 atenuación
};
struct TevMaterialRec {
    uint8_t num_stages, num_texgens, xlu, cull;
    uint32_t alpha_compare, blend, zmode;
    int16_t cregs[4][4];  // PREV, C0, C1, C2 (RGBA, -1024..1023)
    uint8_t kregs[4][4];  // K0..K3
    uint32_t ksel[8];
    uint32_t texgen[8];
    uint32_t dualtex[8];
    TevStageRec stages[16];
    TevMapRec maps[8];
    uint8_t pad[32];
    // Versión 3:
    float tex_mtx[8][2][4];   // matriz de textura por coordenada (filas s y t sobre (s, t, 1, 1))
    uint8_t tex_mtx_mode[8];  // 0 sin matriz; 1 + modo de mapeo de NW4R (0 coordenadas, 1/3/4 entorno, 2 proyección)
    uint8_t num_chans, pad2[3];
    ChanRec chans[2];
    // Versión 4 (en la 3 estos bytes van en cero y ref_valid = 0):
    int8_t cam_ref[8];        // TexMtxEffect.ref_camera de cada matriz de textura (-1 = la cámara actual)
    int8_t light_ref[8];      // TexMtxEffect.ref_light (-1 = ninguna)
    int8_t light_set;         // ResMatMiscData.light_set_idx (-1 = sin conjunto de luces)
    int8_t fog;               // ResMatMiscData.fog_idx
    uint8_t ref_valid;        // bit0: los campos de arriba vienen del conversor
    uint8_t pad4;
    uint8_t pad3[184];
};
#pragma pack(pop)
static_assert(sizeof(ModelVertexV2) == 36, "vértice de 36 bytes");
static_assert(sizeof(ModelVertex) == 44, "vértice de 44 bytes");
static_assert(sizeof(ModelBone) == 64, "hueso de 64 bytes");
static_assert(sizeof(ModelSubmesh) == 32, "submalla de 32 bytes");
static_assert(sizeof(TevStageRec) == 12, "etapa de 12 bytes");
static_assert(sizeof(ChanRec) == 20, "canal de 20 bytes");
static_assert(sizeof(TevMaterialRec) == 1024, "material de 1024 bytes");
constexpr size_t kMaterialV2Size = 512;

enum ModelFlags : uint32_t { MODEL_HAS_NORMALS = 1, MODEL_HAS_COLORS = 2, MODEL_SKINNED = 4 };

struct ModelView {
    const ModelVertex* vertices = nullptr;  // versión 3; las anteriores se copian a este formato (owned_vertices)
    uint32_t vertex_count = 0;
    const uint32_t* indices = nullptr;
    uint32_t index_count = 0;
    const ModelSubmesh* submeshes = nullptr;
    uint32_t submesh_count = 0;
    const TevMaterialRec* materials = nullptr;  // nullptr en paquetes de versión 1
    uint32_t material_count = 0;
    uint32_t version = 0;
    uint32_t flags = 0;
    float bbox_min[3] = {0, 0, 0};
    float bbox_max[3] = {0, 0, 0};
    const ModelBone* bones = nullptr;
    uint32_t bone_count = 0;
    std::vector<ModelVertex> owned_vertices;     // solo para paquetes de versión 1 y 2
    std::vector<TevMaterialRec> owned_materials;
};

// Interpreta una entrada de modelo (sin copiar datos en la versión 3). false si está dañada.
bool model_view(const Pack& pack, const PackEntry& e, ModelView& out);

// Animación horneada para un modelo: por cuadro, una matriz 3x4 por hueso (la de piel: pose animada por la inversa
// de la de reposo), lista para el vértice.
struct AnimView {
    uint64_t model_key = 0;
    uint32_t frames = 0, bones = 0;
    bool loop = false;
    float fps = 60.0f;
    const float* matrices = nullptr;  // frames * bones * 12
    const float* frame(uint32_t f) const { return matrices + size_t(f % (frames ? frames : 1)) * bones * 12; }
};
bool anim_view(const Pack& pack, const PackEntry& e, AnimView& out);
// Entradas KIND_ANIM horneadas para el modelo de esa clave (índices del paquete).
std::vector<uint32_t> anims_for(const Pack& pack, uint64_t model_key);

// Animación de material o visibilidad horneada (CMAN; choya/anim_mat.py). Los destinos ya son índices del modelo.
enum MatAnimType : uint32_t { MATANIM_SRT = 1, MATANIM_CLR = 2, MATANIM_PAT = 3, MATANIM_VIS = 4 };
#pragma pack(push, 1)
struct MatAnimTrack {
    uint16_t target;   // material (en VIS0, submalla)
    uint8_t slot;      // coordenada, objetivo de color (0 CLR0, 1 CLR1, 2 AMB0, 3 AMB1, 4-6 C0-C2, 7-10 K0-K3) o unidad
    uint8_t flags;     // bit0: constante (un solo valor)
    uint32_t offset;   // desde el principio del bloque
    uint32_t count;
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(MatAnimTrack) == 16, "pista de 16 bytes");
struct MatAnimView {
    uint32_t type = 0, frames = 0;
    uint64_t model_key = 0;
    bool loop = false;
    float fps = 60.0f;
    const MatAnimTrack* tracks = nullptr;
    uint32_t track_count = 0;
    const uint64_t* keys = nullptr;  // PAT0: claves de textura
    uint32_t key_count = 0;
    const uint8_t* base = nullptr;
    // Valor de la pista en el cuadro f (ya acotado o en bucle por quien llama): puntero a 8 float (SRT), 4 u8 (CLR),
    // 1 u16 (PAT) o 1 u8 (VIS).
    const void* value(const MatAnimTrack& t, uint32_t f) const;
};
bool mat_anim_view(const Pack& pack, const PackEntry& e, MatAnimView& out);

// Escena SCN0 horneada (CSCN; choya/scn0.py).
#pragma pack(push, 1)
struct SceneLightSet {
    uint16_t ref;
    int16_t amb;        // índice en la tabla de ambientes (-1 = blanco)
    uint8_t count;
    uint8_t pad[3];
    int16_t light[8];   // índices en la tabla de luces
    uint8_t pad2[8];
};
struct SceneAmb {
    uint16_t ref;
    uint8_t flags, constant;
    uint32_t offset, count;
};
struct SceneLight {
    uint16_t ref;
    int16_t spec;       // índice de su luz especular (-1 = ninguna)
    uint8_t type;       // 0 punto, 1 direccional, 2 foco, 3 especular
    uint8_t flags;      // 1 color, 2 alfa
    uint8_t constant, pad;
    uint32_t offset, count;
};
struct SceneLightFrame {
    float pos[3], dir[3];
    uint8_t color[4];
    float a[3], k[3];
    uint32_t enabled;
    float shininess;
    uint32_t pad;
};
struct SceneCamera {
    uint16_t ref;
    uint8_t proj, constant;
    uint32_t offset, count, pad;
};
struct SceneCameraFrame {
    float view[12];     // 3x4 por filas, mundo -> cámara
    float fovy_or_height, aspect, near_z, far_z;
};
#pragma pack(pop)
static_assert(sizeof(SceneLightSet) == 32 && sizeof(SceneAmb) == 12 && sizeof(SceneLight) == 16 &&
                  sizeof(SceneCamera) == 16 && sizeof(SceneLightFrame) == 64 && sizeof(SceneCameraFrame) == 64,
              "tamaños de CSCN");
struct SceneView {
    uint32_t frames = 0;
    bool loop = false;
    float fps = 60.0f;
    const SceneLightSet* sets = nullptr;
    const SceneAmb* ambs = nullptr;
    const SceneLight* lights = nullptr;
    const SceneCamera* cameras = nullptr;
    uint32_t set_count = 0, amb_count = 0, light_count = 0, camera_count = 0, fog_count = 0;
    const uint8_t* base = nullptr;
    const uint8_t* amb_color(uint32_t i, uint32_t f) const;
    const SceneLightFrame& light(uint32_t i, uint32_t f) const;
    const SceneCameraFrame& camera(uint32_t i, uint32_t f) const;
    int find_set(int ref) const;     // índice en sets con ese refNumber (-1 si no hay)
    int find_light(int ref) const;
    int find_camera(int ref) const;
};
bool scene_view(const Pack& pack, const PackEntry& e, SceneView& out);

// Grupo de animaciones que se reproducen juntas, con el mismo reloj: las que comparten nombre (como las enlaza un
// juego de NW4R: "wait" de CHR0, SRT0, CLR0, PAT0 y VIS0). Se arma una vez al cargar el modelo; durante el cuadro no
// se buscan nombres.
struct AnimGroup {
    std::string name;
    int32_t chr = -1;                // entrada KIND_ANIM (-1 = sin huesos)
    std::vector<uint32_t> mats;      // entradas KIND_MATANIM
    uint32_t frames = 0;             // la más larga del grupo
    bool loop = false;
};
std::vector<AnimGroup> anim_groups_for(const Pack& pack, uint64_t model_key);
// Entradas KIND_SCENE ordenadas: primero las del mismo archivo que el modelo (prefijo de ruta más largo).
std::vector<uint32_t> scenes_for(const Pack& pack, const char* model_name);

}  // namespace wl
