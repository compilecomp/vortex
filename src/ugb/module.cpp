#include "vortex/ugb/module.hpp"

#include <cstring>

namespace vortex::ugb {

// ---------------------------------------------------------------------------
// IcSlot — atomic IC transitions (docs/tier-t0.md section 3).
// ---------------------------------------------------------------------------

const IcEntry* IcSlot::lookup(uint32_t klass_id) const noexcept {
    switch (state) {
    case IcState::Monomorphic:
        return mono.klass_or_shape_id == klass_id ? &mono : nullptr;
    case IcState::Polymorphic:
        for (uint8_t i = 0; i < poly_count; ++i) {
            if (poly[i].klass_or_shape_id == klass_id) return &poly[i];
        }
        return nullptr;
    case IcState::Megamorphic:
    case IcState::Uninitialized:
    default:
        return nullptr;
    }
}

void IcSlot::record_hit(uint32_t klass_id, uint32_t target) noexcept {
    switch (state) {
    case IcState::Uninitialized:
        state = IcState::Monomorphic;
        mono = IcEntry{klass_id, target};
        break;
    case IcState::Monomorphic:
        if (mono.klass_or_shape_id == klass_id) return;
        state = IcState::Polymorphic;
        poly_count = 0;
        poly[poly_count++] = mono;
        poly[poly_count++] = IcEntry{klass_id, target};
        break;
    case IcState::Polymorphic:
        if (poly_count < 4) {
            for (uint8_t i = 0; i < poly_count; ++i) {
                if (poly[i].klass_or_shape_id == klass_id) return;
            }
            poly[poly_count++] = IcEntry{klass_id, target};
        } else {
            state = IcState::Megamorphic;
        }
        break;
    case IcState::Megamorphic:
        break;
    }
}

// ---------------------------------------------------------------------------
// UGBModule — token interning and constants.
// ---------------------------------------------------------------------------

uint32_t UGBModule::intern_class(const std::string& name) {
    for (uint32_t i = 0; i < classes.size(); ++i) {
        if (classes[i].name == name) return i;
    }
    classes.push_back(ClassTokenInfo{name});
    return static_cast<uint32_t>(classes.size() - 1);
}

uint32_t UGBModule::intern_field(const std::string& name, uint32_t klass_token) {
    for (uint32_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name && fields[i].klass_token == klass_token) return i;
    }
    fields.push_back(FieldTokenInfo{name, klass_token});
    return static_cast<uint32_t>(fields.size() - 1);
}

uint32_t UGBModule::intern_method(const std::string& name) {
    // Token 0 is reserved ("none"), so real tokens start at 1.
    for (uint32_t i = 0; i < methods.size(); ++i) {
        if (methods[i].name == name) return i + 1;
    }
    methods.push_back(MethodTokenInfo{name});
    return static_cast<uint32_t>(methods.size());  // token = index + 1
}

uint32_t UGBModule::intern_builtin(const std::string& name) {
    for (uint32_t i = 0; i < builtins.size(); ++i) {
        if (builtins[i].name == name) return i;
    }
    builtins.push_back(BuiltinTokenInfo{name});
    return static_cast<uint32_t>(builtins.size() - 1);
}

uint32_t UGBModule::intern_i64(int64_t v) {
    for (uint32_t i = 0; i < constants.size(); ++i) {
        if (constants[i].kind == Constant::Kind::Int64 && constants[i].i64 == v) {
            return i;
        }
    }
    constants.push_back(Constant{Constant::Kind::Int64, v, 0.0, {}});
    return static_cast<uint32_t>(constants.size() - 1);
}

uint32_t UGBModule::intern_f64(double v) {
    for (uint32_t i = 0; i < constants.size(); ++i) {
        if (constants[i].kind == Constant::Kind::Float64 && constants[i].f64 == v) {
            return i;
        }
    }
    Constant c;
    c.kind = Constant::Kind::Float64;
    c.f64 = v;
    constants.push_back(c);
    return static_cast<uint32_t>(constants.size() - 1);
}

uint32_t UGBModule::intern_string(const std::string& s) {
    for (uint32_t i = 0; i < constants.size(); ++i) {
        if (constants[i].kind == Constant::Kind::String && constants[i].str == s) {
            return i;
        }
    }
    Constant c;
    c.kind = Constant::Kind::String;
    c.str = s;
    constants.push_back(c);
    return static_cast<uint32_t>(constants.size() - 1);
}

int32_t UGBModule::find_method(const std::string& name) const noexcept {
    for (size_t i = 0; i < method_table.size(); ++i) {
        if (method_table[i].name == name) return static_cast<int32_t>(i);
    }
    return -1;
}

TaggedValue UGBModule::materialize_constant(uint32_t index) const {
    if (index >= constants.size()) return TaggedValue::undefined();
    const Constant& c = constants[index];
    switch (c.kind) {
    case Constant::Kind::Int64:
        return TaggedValue::smi(c.i64);
    case Constant::Kind::Float64:
        return TaggedValue::smi(static_cast<int64_t>(c.f64));  // M0: truncated
    case Constant::Kind::String:
        return TaggedValue::undefined();  // boxed strings arrive with the GC heap
    case Constant::Kind::MethodRef:
        return TaggedValue::undefined();
    }
    return TaggedValue::undefined();
}

// ---------------------------------------------------------------------------
// Binary encoding (docs/ugb.md sections 8 and 18).
//
// Instruction encoding:
//   opcode: u16 | flags: u8 | dst: u16 | src_count: u8 | srcs: u16[] | meta: u32?
//
// Module encoding (little-endian):
//   magic "UGB\0" | version_major u16 | version_minor u16 | language_id u32
//   language_name (u16 len + bytes)
//   constants: u32 count, each: kind u8 + payload
//   classes / fields / methods / builtins: u32 count, each: name (+owner u32)
//   method_table: u32 count, each: name, id u32, register_count u16,
//                 arg_count u16, code u32 length + bytes
// ---------------------------------------------------------------------------

namespace {

void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_f64(std::vector<uint8_t>& out, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    put_u64(out, bits);
}

void put_string(std::vector<uint8_t>& out, const std::string& s) {
    put_u16(out, static_cast<uint16_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    bool ok = true;

    bool take(size_t n) {
        if (pos + n > size) {
            ok = false;
            return false;
        }
        pos += n;
        return true;
    }
    uint8_t u8() {
        if (!take(1)) return 0;
        return data[pos - 1];
    }
    uint16_t u16() {
        if (!take(2)) return 0;
        return static_cast<uint16_t>(data[pos - 2] | (data[pos - 1] << 8));
    }
    uint32_t u32() {
        if (!take(4)) return 0;
        return static_cast<uint32_t>(data[pos - 4]) |
               (static_cast<uint32_t>(data[pos - 3]) << 8) |
               (static_cast<uint32_t>(data[pos - 2]) << 16) |
               (static_cast<uint32_t>(data[pos - 1]) << 24);
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(u8()) << (8 * i);
        return v;
    }
    double f64() {
        const uint64_t bits = u64();
        double v = 0;
        std::memcpy(&v, &bits, 8);
        return v;
    }
    std::string string() {
        const uint16_t len = u16();
        if (!ok || !take(len)) return {};
        return std::string(reinterpret_cast<const char*>(data + pos - len), len);
    }
};

}  // namespace

void append_instruction(std::vector<uint8_t>& code, Op opcode, uint16_t dst,
                        std::initializer_list<uint16_t> srcs, bool has_meta,
                        uint32_t meta) {
    put_u16(code, static_cast<uint16_t>(opcode));
    code.push_back(has_meta ? FLAG_HAS_META : FLAG_NONE);
    put_u16(code, dst);
    code.push_back(static_cast<uint8_t>(srcs.size()));
    for (uint16_t s : srcs) put_u16(code, s);
    if (has_meta) put_u32(code, meta);
}

bool InstructionStream::decode_at(size_t& offset, Instruction& out) const noexcept {
    Reader r{data_, size_, offset, true};
    out.opcode = static_cast<Op>(r.u16());
    out.flags = r.u8();
    out.dst = r.u16();
    const uint8_t count = r.u8();
    if (!r.ok) return false;
    if (count > 8) return false;
    out.srcs.clear();
    for (uint8_t i = 0; i < count; ++i) out.srcs.push_back(r.u16());
    out.has_meta = (out.flags & FLAG_HAS_META) != 0;
    out.meta = out.has_meta ? r.u32() : 0;
    if (!r.ok) return false;
    offset = r.pos;
    return true;
}

bool InstructionStream::next_offset(size_t from, uint32_t n, size_t& out) const noexcept {
    size_t off = from;
    Instruction ins;
    for (uint32_t i = 0; i < n; ++i) {
        if (off >= size_) return false;
        if (!decode_at(off, ins)) return false;
    }
    out = off;
    return true;
}

std::vector<uint8_t> encode_module(const UGBModule& m) {
    std::vector<uint8_t> out;
    out.push_back('U');
    out.push_back('G');
    out.push_back('B');
    out.push_back(0);
    put_u16(out, m.version_major);
    put_u16(out, m.version_minor);
    put_u32(out, m.language_id);
    put_string(out, m.language_name);

    // constants
    put_u32(out, static_cast<uint32_t>(m.constants.size()));
    for (const Constant& c : m.constants) {
        out.push_back(static_cast<uint8_t>(c.kind));
        switch (c.kind) {
        case Constant::Kind::Int64: put_u64(out, static_cast<uint64_t>(c.i64)); break;
        case Constant::Kind::Float64: put_f64(out, c.f64); break;
        case Constant::Kind::String: put_string(out, c.str); break;
        case Constant::Kind::MethodRef: put_u32(out, 0); break;
        }
    }

    auto put_names = [&](const auto& vec) {
        put_u32(out, static_cast<uint32_t>(vec.size()));
        for (const auto& e : vec) put_string(out, e.name);
    };
    put_names(m.classes);
    put_u32(out, static_cast<uint32_t>(m.fields.size()));
    for (const auto& f : m.fields) {
        put_string(out, f.name);
        put_u32(out, f.klass_token);
    }
    put_names(m.methods);
    put_names(m.builtins);

    // methods
    put_u32(out, static_cast<uint32_t>(m.method_table.size()));
    for (const UGBMethod& mt : m.method_table) {
        put_string(out, mt.name);
        put_u32(out, mt.id);
        put_u16(out, mt.register_count);
        put_u16(out, mt.arg_count);
        put_u32(out, static_cast<uint32_t>(mt.code.size()));
        out.insert(out.end(), mt.code.begin(), mt.code.end());
    }
    return out;
}

bool decode_module(const uint8_t* data, size_t size, UGBModule& out,
                   DecodeErrorInfo& error) {
    Reader r{data, size, 0, true};
    if (size < 4 || data[0] != 'U' || data[1] != 'G' || data[2] != 'B' ||
        data[3] != 0) {
        error = {"bad magic", 0};
        return false;
    }
    r.take(4);
    out = UGBModule{};
    out.version_major = r.u16();
    out.version_minor = r.u16();
    out.language_id = r.u32();
    out.language_name = r.string();
    if (!r.ok) {
        error = {"truncated header", r.pos};
        return false;
    }

    const uint32_t const_count = r.u32();
    for (uint32_t i = 0; i < const_count && r.ok; ++i) {
        Constant c;
        c.kind = static_cast<Constant::Kind>(r.u8());
        switch (c.kind) {
        case Constant::Kind::Int64: c.i64 = static_cast<int64_t>(r.u64()); break;
        case Constant::Kind::Float64: c.f64 = r.f64(); break;
        case Constant::Kind::String: c.str = r.string(); break;
        case Constant::Kind::MethodRef: (void)r.u32(); break;
        }
        out.constants.push_back(std::move(c));
    }

    auto read_names = [&](auto& vec) {
        using E = typename std::decay_t<decltype(vec)>::value_type;
        const uint32_t n = r.u32();
        vec.clear();
        for (uint32_t i = 0; i < n && r.ok; ++i) {
            E e{};
            e.name = r.string();
            vec.push_back(std::move(e));
        }
    };
    read_names(out.classes);
    const uint32_t field_count = r.u32();
    for (uint32_t i = 0; i < field_count && r.ok; ++i) {
        FieldTokenInfo f;
        f.name = r.string();
        f.klass_token = r.u32();
        out.fields.push_back(std::move(f));
    }
    read_names(out.methods);
    read_names(out.builtins);

    const uint32_t method_count = r.u32();
    for (uint32_t i = 0; i < method_count && r.ok; ++i) {
        UGBMethod mt;
        mt.name = r.string();
        mt.id = r.u32();
        mt.register_count = r.u16();
        mt.arg_count = r.u16();
        const uint32_t code_len = r.u32();
        if (!r.ok || !r.take(code_len)) break;
        mt.code.assign(data + r.pos - code_len, data + r.pos);
        out.method_table.push_back(std::move(mt));
    }

    if (!r.ok) {
        error = {"truncated module", r.pos};
        return false;
    }
    error = {};
    return true;
}

}  // namespace vortex::ugb
