// Choya Wii Converter - el mando de la consola leído como en Nebula.
//
// Nebula (el port de Super Mario Galaxy) usa SDL2 con el mapeo de PortMaster (SDL_GAMECONTROLLERCONFIG). Los mapeos
// de PortMaster numeran los botones como SDL2 en Linux: primero los códigos desde BTN_JOYSTICK hasta KEY_MAX y luego
// los de abajo, y los ejes de ABS_X en adelante sin los hats. El runtime usa SDL3, que numera distinto, y con el mismo
// mapeo los botones salían cambiados (A en X, B en L1). Aquí se lee el dispositivo de /dev/input directamente, se
// numera como SDL2 y se aplica el mapeo de PortMaster tal cual: los botones quedan como en Nebula.
#include "choya_mando.h"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" void choya_gles_log(const char* line);  // choya-gles.txt (choya_gles.cpp)

namespace {

constexpr size_t kBits = 8 * sizeof(unsigned long);
bool TestBit(const std::vector<unsigned long>& bits, int n) {
    return (bits[n / kBits] >> (n % kBits)) & 1UL;
}

// Una entrada del mapeo: botón, eje o hat de SDL2, con media escala (+/-) e inversión (~).
struct Source {
    enum Kind { None, Button, Axis, Hat } kind = None;
    int code = -1;      // código de evdev (botón o eje)
    int hatMask = 0;    // 1 arriba, 2 derecha, 4 abajo, 8 izquierda
    int half = 0;       // +1 / -1: solo la mitad del eje
    bool invert = false;
};

struct Device {
    int fd = -1;
    std::string name;
    std::vector<int> buttonCodes;  // índice de botón de SDL2 -> código
    std::vector<int> axisCodes;    // índice de eje de SDL2 -> código
    input_absinfo abs[ABS_CNT] = {};
    bool hasAbs[ABS_CNT] = {};
    // Salidas del game controller.
    Source south, east, west, north, back, guide, start, lshoulder, rshoulder, dpup, dpdown, dpleft, dpright;
    Source leftx, lefty, ltrigger, rtrigger;
};

Device g_dev;
std::vector<unsigned long> g_prevKeys;
int g_pressLogs = 0;
std::once_flag g_once;
bool g_ok = false;

void Log(const std::string& line) {
    choya_gles_log(line.c_str());
}

// GUID de SDL2 para Linux: bus, crc del nombre (PortMaster lo deja en 0), vendor, product y version (16 bits LE).
std::string Guid(const input_id& id) {
    char out[33];
    const unsigned short v[8] = {id.bustype, 0, id.vendor, 0, id.product, 0, id.version, 0};
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 4, 5, "%02x%02x", v[i] & 0xFF, v[i] >> 8);
    return out;
}
bool SameGuid(const std::string& a, const std::string& b) {
    if (a.size() != 32 || b.size() != 32) return false;
    for (int i = 0; i < 32; ++i) {
        if (i >= 4 && i < 8) continue;  // el crc del nombre
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

Source ParseSource(const Device& d, std::string v) {
    Source s;
    if (!v.empty() && (v[0] == '+' || v[0] == '-')) {
        s.half = v[0] == '+' ? 1 : -1;
        v.erase(0, 1);
    }
    if (!v.empty() && v.back() == '~') {
        s.invert = true;
        v.pop_back();
    }
    if (v.size() < 2) return s;
    if (v[0] == 'b') {
        const int i = std::atoi(v.c_str() + 1);
        if (i >= 0 && i < static_cast<int>(d.buttonCodes.size())) {
            s.kind = Source::Button;
            s.code = d.buttonCodes[i];
        }
    } else if (v[0] == 'a') {
        const int i = std::atoi(v.c_str() + 1);
        if (i >= 0 && i < static_cast<int>(d.axisCodes.size())) {
            s.kind = Source::Axis;
            s.code = d.axisCodes[i];
        }
    } else if (v[0] == 'h') {
        const size_t dot = v.find('.');
        if (dot != std::string::npos && std::atoi(v.c_str() + 1) == 0) {
            s.kind = Source::Hat;
            s.hatMask = std::atoi(v.c_str() + dot + 1);
        }
    }
    return s;
}

std::string Describe(const Source& s) {
    char buf[48];
    switch (s.kind) {
    case Source::Button: std::snprintf(buf, sizeof(buf), "boton %d", s.code); break;
    case Source::Axis: std::snprintf(buf, sizeof(buf), "eje %d%s", s.code, s.half > 0 ? "+" : s.half < 0 ? "-" : ""); break;
    case Source::Hat: std::snprintf(buf, sizeof(buf), "hat %d", s.hatMask); break;
    default: return "-";
    }
    return buf;
}

bool ApplyMapping(Device& d, const std::string& line) {
    size_t pos = 0;
    int field = 0;
    while (pos <= line.size()) {
        size_t comma = line.find(',', pos);
        if (comma == std::string::npos) comma = line.size();
        const std::string item = line.substr(pos, comma - pos);
        pos = comma + 1;
        if (field++ < 2) continue;  // guid y nombre
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        std::string key = item.substr(0, colon);
        const std::string value = item.substr(colon + 1);
        if (!key.empty() && (key[0] == '+' || key[0] == '-')) key.erase(0, 1);
        const Source s = ParseSource(d, value);
        if (key == "a") d.south = s;
        else if (key == "b") d.east = s;
        else if (key == "x") d.west = s;
        else if (key == "y") d.north = s;
        else if (key == "back") d.back = s;
        else if (key == "guide") d.guide = s;
        else if (key == "start") d.start = s;
        else if (key == "leftshoulder") d.lshoulder = s;
        else if (key == "rightshoulder") d.rshoulder = s;
        else if (key == "dpup") d.dpup = s;
        else if (key == "dpdown") d.dpdown = s;
        else if (key == "dpleft") d.dpleft = s;
        else if (key == "dpright") d.dpright = s;
        else if (key == "leftx") d.leftx = s;
        else if (key == "lefty") d.lefty = s;
        else if (key == "lefttrigger") d.ltrigger = s;
        else if (key == "righttrigger") d.rtrigger = s;
        if (comma == line.size()) break;
    }
    return d.south.kind != Source::None || d.east.kind != Source::None;
}

void Open() {
    const char* env = std::getenv("SDL_GAMECONTROLLERCONFIG");
    if (env == nullptr || *env == '\0') {
        Log("mando: sin SDL_GAMECONTROLLERCONFIG (PortMaster); se usa SDL3");
        return;
    }
    std::vector<std::string> lines;
    for (const char* p = env; *p;) {
        const char* end = std::strchr(p, '\n');
        const size_t len = end ? static_cast<size_t>(end - p) : std::strlen(p);
        std::string l(p, len);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty() && l[0] != '#') lines.push_back(l);
        p += len + (end ? 1 : 0);
    }
    for (int n = 0; n < 32; ++n) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/input/event%d", n);
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        input_id id{};
        ioctl(fd, EVIOCGID, &id);
        const std::string guid = Guid(id);
        // Primero por nombre, luego por GUID (sin el crc), como SDL2.
        const std::string* match = nullptr;
        for (const std::string& l : lines) {
            const size_t c1 = l.find(',');
            const size_t c2 = c1 == std::string::npos ? c1 : l.find(',', c1 + 1);
            if (c2 != std::string::npos && l.compare(c1 + 1, c2 - c1 - 1, name) == 0 && SameGuid(l.substr(0, c1), guid)) {
                match = &l;
                break;
            }
        }
        for (const std::string& l : lines) {
            if (match) break;
            if (SameGuid(l.substr(0, l.find(',')), guid)) match = &l;
        }
        if (match == nullptr) {
            close(fd);
            continue;
        }
        Device& d = g_dev;
        d.fd = fd;
        d.name = name;
        std::vector<unsigned long> keybit(KEY_CNT / kBits + 1), absbit(ABS_CNT / kBits + 1);
        ioctl(fd, EVIOCGBIT(EV_KEY, keybit.size() * sizeof(unsigned long)), keybit.data());
        ioctl(fd, EVIOCGBIT(EV_ABS, absbit.size() * sizeof(unsigned long)), absbit.data());
        // Numeración de SDL2 (SDL_sysjoystick.c, ConfigJoystick).
        for (int i = BTN_JOYSTICK; i < KEY_MAX; ++i) if (TestBit(keybit, i)) d.buttonCodes.push_back(i);
        for (int i = 0; i < BTN_JOYSTICK; ++i) if (TestBit(keybit, i)) d.buttonCodes.push_back(i);
        for (int i = 0; i < ABS_MAX; ++i) {
            if (i == ABS_HAT0X) {
                i = ABS_HAT3Y;
                continue;
            }
            if (TestBit(absbit, i)) d.axisCodes.push_back(i);
        }
        for (int i = 0; i < ABS_CNT; ++i) {
            if (TestBit(absbit, i) && ioctl(fd, EVIOCGABS(i), &d.abs[i]) >= 0) d.hasAbs[i] = true;
        }
        if (!ApplyMapping(d, *match)) {
            Log("mando: el mapeo de PortMaster no tiene botones para " + d.name);
            close(fd);
            d = Device{};
            continue;
        }
        g_ok = true;
        Log("mando: " + std::string(path) + " '" + d.name + "' guid " + guid + ", mapeo de PortMaster como en SDL2");
        std::string codes = "mando: codigos de botones (orden de SDL2):";
        for (size_t i = 0; i < d.buttonCodes.size(); ++i) codes += " b" + std::to_string(i) + "=" + std::to_string(d.buttonCodes[i]);
        Log(codes);
        Log("mando: abajo(a) " + Describe(d.south) + ", derecha(b) " + Describe(d.east) + ", izquierda(x) " +
            Describe(d.west) + ", arriba(y) " + Describe(d.north) + ", L1 " + Describe(d.lshoulder) + ", R1 " +
            Describe(d.rshoulder) + ", L2 " + Describe(d.ltrigger) + ", R2 " + Describe(d.rtrigger) + ", select " +
            Describe(d.back) + ", start " + Describe(d.start) + ", menu " + Describe(d.guide));
        return;
    }
    Log("mando: ningun /dev/input coincide con el mapeo de PortMaster; se usa SDL3");
}

// Eje normalizado a -1..1 (o 0..1 si el eje solo va de 0 hacia arriba).
float AxisValue(const Device& d, int code) {
    if (code < 0 || code >= ABS_CNT || !d.hasAbs[code]) return 0.0f;
    input_absinfo info{};
    if (ioctl(d.fd, EVIOCGABS(code), &info) < 0) return 0.0f;
    const float range = static_cast<float>(info.maximum - info.minimum);
    if (range <= 0.0f) return 0.0f;
    return (static_cast<float>(info.value - info.minimum) / range) * 2.0f - 1.0f;
}

float Value(const Device& d, const Source& s, const std::vector<unsigned long>& keys, bool asTrigger) {
    float v = 0.0f;
    switch (s.kind) {
    case Source::Button:
        v = TestBit(keys, s.code) ? 1.0f : 0.0f;
        break;
    case Source::Hat: {
        const float x = AxisValue(d, ABS_HAT0X), y = AxisValue(d, ABS_HAT0Y);
        const bool on = ((s.hatMask & 1) && y < -0.5f) || ((s.hatMask & 2) && x > 0.5f) ||
                        ((s.hatMask & 4) && y > 0.5f) || ((s.hatMask & 8) && x < -0.5f);
        v = on ? 1.0f : 0.0f;
        break;
    }
    case Source::Axis:
        v = AxisValue(d, s.code);
        if (s.invert) v = -v;
        if (s.half > 0) v = v > 0 ? v : 0.0f;
        else if (s.half < 0) v = v < 0 ? -v : 0.0f;
        else if (asTrigger) v = (v + 1.0f) * 0.5f;  // eje completo como gatillo: -1..1 -> 0..1
        break;
    default:
        break;
    }
    return v;
}

}  // namespace

extern "C" bool choya_mando_leer(ChoyaMando* out) {
    std::call_once(g_once, Open);
    if (!g_ok || out == nullptr) return false;
    const Device& d = g_dev;
    std::vector<unsigned long> keys(KEY_CNT / kBits + 1);
    if (ioctl(d.fd, EVIOCGKEY(keys.size() * sizeof(unsigned long)), keys.data()) < 0) return false;
    // Los primeros botones que se aprietan, con su código y a qué van (para revisar el mapeo en el resumen).
    if (g_prevKeys.size() == keys.size() && g_pressLogs < 40) {
        for (int code : d.buttonCodes) {
            if (TestBit(keys, code) && !TestBit(g_prevKeys, code)) {
                ++g_pressLogs;
                const auto is = [code](const Source& s) { return s.kind == Source::Button && s.code == code; };
                const char* role = is(d.east) ? "derecha (b) = Wii A" : is(d.south) ? "abajo (a) = Wii B" :
                                   is(d.north) ? "arriba (y) = Wii 1" : is(d.west) ? "izquierda (x) = Wii 2" :
                                   is(d.rshoulder) ? "R1 = agitar" : is(d.lshoulder) ? "L1 = Wii -" :
                                   is(d.back) ? "select = Wii -" : is(d.start) ? "start = Wii +" :
                                   is(d.guide) ? "menu" : is(d.ltrigger) ? "L2 = Wii -" : is(d.rtrigger) ? "R2" : "sin uso";
                Log("mando: boton " + std::to_string(code) + " -> " + role);
            }
        }
    }
    g_prevKeys = keys;
    const auto on = [&](const Source& s) { return Value(d, s, keys, false) > 0.5f; };
    out->south = on(d.south);
    out->east = on(d.east);
    out->west = on(d.west);
    out->north = on(d.north);
    out->back = on(d.back);
    out->guide = on(d.guide);
    out->start = on(d.start);
    out->leftshoulder = on(d.lshoulder);
    out->rightshoulder = on(d.rshoulder);
    out->dpup = on(d.dpup);
    out->dpdown = on(d.dpdown);
    out->dpleft = on(d.dpleft);
    out->dpright = on(d.dpright);
    out->leftx = Value(d, d.leftx, keys, false);
    out->lefty = Value(d, d.lefty, keys, false);
    out->lefttrigger = Value(d, d.ltrigger, keys, true);
    out->righttrigger = Value(d, d.rtrigger, keys, true);
    return true;
}

extern "C" const char* choya_mando_nombre() {
    std::call_once(g_once, Open);
    return g_ok ? g_dev.name.c_str() : "";
}
