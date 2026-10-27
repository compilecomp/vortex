// vx — the Vortex driver.
//
//   vx run <file.ugb|file.mini> --entry NAME [--args v1 v2 ...]
//   vx asm <in.ugb-text> <out.ugb>
//   vx dis <file.ugb>
//   vx stats <file.ugb> --entry NAME
//   vx version
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "vortex/frontends/reference/emitter.hpp"
#include "vortex/gc/icggc.hpp"
#include "vortex/infra/observability.hpp"
#include "vortex/support/tier.hpp"
#include "vortex/ugb/assembler.hpp"
#include "vortex/ugb/disassembler.hpp"
#include "vortex/ugb/verifier.hpp"
#include "vortex/vm/interpreter.hpp"
#include "vortex/vortex.hpp"

namespace {

/// Length of the "--jitdump=" flag prefix (strncmp bound in main's parse).
constexpr size_t kJitdumpFlagPrefixLen = 10;

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string extension_of(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

/// Loads a module from .ugb text (assembles) or binary (decodes, detected by
/// the "UGB\0" magic written by `vx asm`). `path` is reserved for origin
/// diagnostics in load failures; the current branches report via `source`.
bool load_module([[maybe_unused]] const std::string& path,
                 const std::string& source,
                 vortex::ugb::UGBModule& module, std::string& error) {
    if (source.size() >= 4 && source[0] == 'U' && source[1] == 'G' &&
        source[2] == 'B' && source[3] == 0) {
        vortex::ugb::DecodeErrorInfo err;
        if (!vortex::ugb::decode_module(
                reinterpret_cast<const uint8_t*>(source.data()), source.size(),
                module, err)) {
            error = "decode: " + err.message + " @+" +
                    std::to_string(err.offset);
            return false;
        }
        return true;
    }
    auto assembled = vortex::ugb::assemble_module(source);
    if (!assembled) {
        error = "assemble: " + assembled.error().format();
        return false;
    }
    module = std::move(*assembled);
    return true;
}

int cmd_run(const std::string& path, const std::string& entry,
            const std::vector<std::string>& args,
            const std::string& jitdump_path, bool telemetry = false) {
    std::string source;
    if (!read_file(path, source)) {
        std::cerr << "vx: cannot read '" << path << "'\n";
        return 2;
    }

    // Language frontends lower into UGB; the engine only ever sees the module.
    vortex::ugb::UGBModule module;
    const std::string ext = extension_of(path);
    if (ext == "mini") {
        auto compiled = vortex::frontends::reference::compile_mini(source);
        if (!compiled) {
            std::cerr << "vx: reference frontend: " << compiled.error().format()
                      << "\n";
            return 1;
        }
        module = std::move(*compiled);
    } else if (ext == "ugb") {
        std::string error;
        if (!load_module(path, source, module, error)) {
            std::cerr << "vx: " << error << "\n";
            return 1;
        }
    } else {
        std::cerr << "vx: unsupported input '" << path
                  << "' (expected .ugb or .mini)\n";
        return 2;
    }

    std::vector<vortex::TaggedValue> call_args;
    call_args.reserve(args.size());
    for (const std::string& a : args) {
        call_args.push_back(vortex::TaggedValue::smi(std::atoll(a.c_str())));
    }

    vortex::gc::Heap heap;
    vortex::vm::Interpreter interp(heap);
    interp.register_builtin("print",
                            [](std::span<const vortex::TaggedValue> a, void*) {
                                for (size_t i = 0; i < a.size(); ++i) {
                                    if (i != 0) std::fputs(" ", stdout);
                                    const std::string s = a[i].to_string();
                                    std::fputs(s.c_str(), stdout);
                                }
                                std::fputs("\n", stdout);
                                return vortex::TaggedValue::undefined();
                            });

    // Observability floor (docs/infrastructure/06-observability.md section
    // 7): --jitdump opens the session (jitdump + perf-map) for the process
    // lifetime; the publish hooks feed it, stats prints what a cold run can
    // reach.
    const bool jit = !jitdump_path.empty();
    if (jit) {
        auto started = jitdump_path == "default"
                           ? vortex::infra::jit_session().start_default()
                           : vortex::infra::jit_session().start(
                                 jitdump_path + ".jitdump", jitdump_path + ".map");
        if (!started) {
            std::cerr << "vx: jitdump: " << started.error().format() << "\n";
            return 1;
        }
    }

    auto result = interp.run(module, entry, call_args);
    if (!result) {
        if (jit) (void)vortex::infra::jit_session().stop();
        std::cerr << "vx: runtime: " << result.error().format() << "\n";
        return 1;
    }
    const vortex::vm::InterpStats& st = result->stats;
    std::cout << "=> " << result->value.to_string() << "\n";
    std::cout << "tier T0 | instructions=" << st.instructions_executed
              << " (typed=" << st.typed_instructions_executed
              << ", generic=" << st.generic_instructions_executed
              << ") calls=" << st.calls << " allocations=" << st.allocations
              << " ic_hits=" << st.ic_hits << " ic_misses=" << st.ic_misses
              << " typed_rewrites=" << st.typed_rewrites
              << " generic_rewrites=" << st.generic_rewrites
              << " osr_requests=" << st.osr_requests
              << " safepoints=" << st.safepoint_polls
              << " max_depth=" << st.max_call_depth << "\n";
    if (telemetry) {
        const vortex::gc::HeapStats& hs = heap.stats();
        std::cout << "heap: tlab_refills=" << hs.tlab_refills
                  << " objects=" << hs.objects_allocated
                  << " bytes=" << hs.bytes_allocated
                  << " slow_path=" << hs.slow_path_allocations << "\n";
        const vortex::infra::AllocSampler& sampler =
            vortex::infra::alloc_sampler();
        std::cout << "alloc_sampler: interval=" << sampler.interval_bytes()
                  << " refills=" << sampler.refills_observed()
                  << " samples=" << sampler.total_samples()
                  << " total_refill_bytes=" << sampler.total_refill_bytes()
                  << "\n";
        std::cout << "safepoints: events=0 ttsp_p99_ns=0 ttsp_max_ns=0"
                  << " (the driver's T0 run has no handshake manager - the"
                  << " TTSP ring fills under freeze_all and handshake"
                  << " consumers)\n";
    }
    if (jit) {
        if (auto stopped = vortex::infra::jit_session().stop(); !stopped) {
            std::cerr << "vx: jitdump stop: " << stopped.error().format()
                      << "\n";
        } else {
            std::cout << "jitdump: "
                      << (jitdump_path == "default"
                              ? vortex::infra::JitSession::default_jitdump_path()
                              : jitdump_path + ".jitdump")
                      << " code_loads="
                      << vortex::infra::jit_session().code_loads() << "\n";
        }
    }
    return 0;
}

int cmd_dis(const std::string& path) {
    std::string source;
    if (!read_file(path, source)) {
        std::cerr << "vx: cannot read '" << path << "'\n";
        return 2;
    }
    vortex::ugb::UGBModule module;
    std::string error;
    if (!load_module(path, source, module, error)) {
        std::cerr << "vx: " << error << "\n";
        return 1;
    }
    std::cout << vortex::ugb::disassemble_module(module);
    return 0;
}

int cmd_asm(const std::string& in, const std::string& out_path) {
    std::string source;
    if (!read_file(in, source)) {
        std::cerr << "vx: cannot read '" << in << "'\n";
        return 2;
    }
    auto module = vortex::ugb::assemble_module(source);
    if (!module) {
        std::cerr << "vx: assemble: " << module.error().format() << "\n";
        return 1;
    }
    auto binary = vortex::ugb::encode_module(*module);
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(binary.data()),
              static_cast<std::streamsize>(binary.size()));
    std::cout << "wrote " << binary.size() << " bytes to " << out_path << "\n";
    return 0;
}

int cmd_stats(const std::string& path, const std::string& entry,
              const std::vector<std::string>& args,
              const std::string& jitdump_path) {
    // stats = run with profile-focused output (the run command already prints
    // the counter block; this command adds the observability floor's
    // telemetry summary).
    return cmd_run(path, entry, args, jitdump_path, /*telemetry=*/true);
}

void usage() {
    std::cout
        << "vx — the Vortex engine driver (T0 in M0)\n\n"
        << "usage:\n"
        << "  vx run <file.ugb|file.mini> --entry NAME [--args N ...] [--jitdump[=PATH]]\n"
        << "  vx asm <in.ugb-text> <out.ugb>\n"
        << "  vx dis <file.ugb>\n"
        << "  vx stats <file.ugb> --entry NAME [--args N ...] [--jitdump[=PATH]]\n"
        << "  vx version\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];

    if (cmd == "version") {
        std::cout << "vortex " << vortex::kVersionMajor << "."
                  << vortex::kVersionMinor << "." << vortex::kVersionPatch
                  << " (T0 online; J1-J4 on the roadmap)\n";
        return 0;
    }
    if (cmd == "help" || cmd == "--help") {
        usage();
        return 0;
    }

    if (cmd == "run" || cmd == "stats") {
        if (argc < 3) {
            usage();
            return 2;
        }
        std::string entry = "main";
        std::vector<std::string> args;
        std::string jitdump_path;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
                entry = argv[++i];
            } else if (std::strcmp(argv[i], "--jitdump") == 0) {
                jitdump_path = "default";
            } else if (std::strncmp(argv[i], "--jitdump=",
                                    kJitdumpFlagPrefixLen) == 0) {
                jitdump_path = argv[i] + kJitdumpFlagPrefixLen;
                if (jitdump_path.empty()) {
                    std::cerr << "vx: --jitdump= requires a path"
                              << " (or use bare --jitdump for the default)\n";
                    return 2;
                }
            } else if (std::strcmp(argv[i], "--args") == 0) {
                for (int j = i + 1; j < argc; ++j) args.push_back(argv[j]);
                break;
            }
        }
        return cmd == "run" ? cmd_run(argv[2], entry, args, jitdump_path)
                            : cmd_stats(argv[2], entry, args, jitdump_path);
    }
    if (cmd == "dis" && argc == 3) return cmd_dis(argv[2]);
    if (cmd == "asm" && argc == 4) return cmd_asm(argv[2], argv[3]);

    usage();
    return 2;
}
