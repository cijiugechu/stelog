const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const is_x86_64 = target.result.cpu.arch == .x86_64;

    const c_flags: []const []const u8 = if (optimize == .Debug)
        &.{ "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-g" }
    else
        &.{ "-std=c11", "-O3", "-Wall", "-Wextra", "-Wno-unused-parameter" };

    const cpp_flags: []const []const u8 = if (is_x86_64)
        (if (optimize == .Debug)
            &.{ "-std=c++17", "-DSIMDJSON_EXCEPTIONS=0", "-DSIMDJSON_IMPLEMENTATION_ICELAKE=0", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-g" }
        else
            &.{ "-std=c++17", "-O3", "-DSIMDJSON_EXCEPTIONS=0", "-DSIMDJSON_IMPLEMENTATION_ICELAKE=0", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra" })
    else
        (if (optimize == .Debug)
            &.{ "-std=c++17", "-DSIMDJSON_EXCEPTIONS=0", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-g" }
        else
            &.{ "-std=c++17", "-O3", "-DSIMDJSON_EXCEPTIONS=0", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra" });

    // 1. Static library: simdjson (C++17, cached independently)
    const simdjson_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    simdjson_mod.addIncludePath(b.path("third_party/simdjson"));
    simdjson_mod.addCSourceFile(.{
        .file = b.path("third_party/simdjson/simdjson.cpp"),
        .flags = if (is_x86_64)
            &.{ "-std=c++17", "-O3", "-DSIMDJSON_EXCEPTIONS=0", "-DSIMDJSON_IMPLEMENTATION_ICELAKE=0", "-fno-exceptions", "-fno-rtti" }
        else
            &.{ "-std=c++17", "-O3", "-DSIMDJSON_EXCEPTIONS=0", "-fno-exceptions", "-fno-rtti" },
    });
    const simdjson_lib = b.addLibrary(.{
        .linkage = .static,
        .name = "simdjson",
        .root_module = simdjson_mod,
    });

    // 1b. Static library: simdutf (C++17 SIMD UTF-8/UTF-32, cached independently)
    const simdutf_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    simdutf_mod.addIncludePath(b.path("third_party/simdutf"));
    simdutf_mod.addCSourceFile(.{
        .file = b.path("third_party/simdutf/simdutf.cpp"),
        .flags = if (is_x86_64)
            &.{ "-std=c++17", "-O3", "-DSIMDUTF_IMPLEMENTATION_ICELAKE=0", "-fno-exceptions", "-fno-rtti" }
        else
            &.{ "-std=c++17", "-O3", "-fno-exceptions", "-fno-rtti" },
    });
    const simdutf_lib = b.addLibrary(.{
        .linkage = .static,
        .name = "simdutf",
        .root_module = simdutf_mod,
    });

    // 2. Static library: zstd decompression (pure C, cross-compilable to all targets)
    const zstd_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    zstd_mod.addIncludePath(b.path("third_party/zstd"));
    zstd_mod.addIncludePath(b.path("third_party/zstd/common"));
    zstd_mod.addCSourceFiles(.{
        .files = &.{
            "third_party/zstd/common/debug.c",
            "third_party/zstd/common/entropy_common.c",
            "third_party/zstd/common/error_private.c",
            "third_party/zstd/common/fse_decompress.c",
            "third_party/zstd/common/xxhash.c",
            "third_party/zstd/common/zstd_common.c",
            "third_party/zstd/decompress/huf_decompress.c",
            "third_party/zstd/decompress/zstd_ddict.c",
            "third_party/zstd/decompress/zstd_decompress.c",
            "third_party/zstd/decompress/zstd_decompress_block.c",
        },
        .flags = &.{ "-std=c11", "-O3", "-DZSTD_DISABLE_ASM=1", "-DXXH_NAMESPACE=ZSTD_" },
    });
    const zstd_lib = b.addLibrary(.{
        .linkage = .static,
        .name = "zstd",
        .root_module = zstd_mod,
    });

    // 3. Static library: sqlite3 amalgamation (read-only metadata loader, cross-compilable)
    const sqlite_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    sqlite_mod.addIncludePath(b.path("third_party/sqlite3"));
    sqlite_mod.addCSourceFile(.{
        .file = b.path("third_party/sqlite3/sqlite3.c"),
        .flags = &.{
            "-O2",
            "-DSQLITE_THREADSAFE=1",
            "-DSQLITE_OMIT_LOAD_EXTENSION=1",
            "-DSQLITE_OMIT_DEPRECATED=1",
            "-DSQLITE_DQS=0",
            "-Wno-unused-function",
            "-Wno-unused-parameter",
        },
    });
    const sqlite_lib = b.addLibrary(.{
        .linkage = .static,
        .name = "sqlite3",
        .root_module = sqlite_mod,
    });

    // 3b. Static library: PCRE2 10.45 (8-bit + Unicode + SLJIT, pure C, 4-target cross-compilable)
    const pcre2_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    pcre2_mod.addIncludePath(b.path("third_party/pcre2/src"));
    pcre2_mod.addCSourceFiles(.{
        .files = &.{
            "third_party/pcre2/src/pcre2_auto_possess.c",
            "third_party/pcre2/src/pcre2_chartables.c",
            "third_party/pcre2/src/pcre2_chkdint.c",
            "third_party/pcre2/src/pcre2_compile.c",
            "third_party/pcre2/src/pcre2_compile_class.c",
            "third_party/pcre2/src/pcre2_config.c",
            "third_party/pcre2/src/pcre2_context.c",
            "third_party/pcre2/src/pcre2_dfa_match.c",
            "third_party/pcre2/src/pcre2_error.c",
            "third_party/pcre2/src/pcre2_extuni.c",
            "third_party/pcre2/src/pcre2_find_bracket.c",
            "third_party/pcre2/src/pcre2_jit_compile.c",
            "third_party/pcre2/src/pcre2_maketables.c",
            "third_party/pcre2/src/pcre2_match.c",
            "third_party/pcre2/src/pcre2_match_data.c",
            "third_party/pcre2/src/pcre2_newline.c",
            "third_party/pcre2/src/pcre2_ord2utf.c",
            "third_party/pcre2/src/pcre2_pattern_info.c",
            "third_party/pcre2/src/pcre2_script_run.c",
            "third_party/pcre2/src/pcre2_serialize.c",
            "third_party/pcre2/src/pcre2_string_utils.c",
            "third_party/pcre2/src/pcre2_study.c",
            "third_party/pcre2/src/pcre2_substring.c",
            "third_party/pcre2/src/pcre2_tables.c",
            "third_party/pcre2/src/pcre2_ucd.c",
            "third_party/pcre2/src/pcre2_valid_utf.c",
            "third_party/pcre2/src/pcre2_xclass.c",
        },
        .flags = &.{
            "-std=c11",
            "-O3",
            "-DHAVE_CONFIG_H=1",
            "-DPCRE2_CODE_UNIT_WIDTH=8",
            "-DPCRE2_STATIC=1",
            "-Wno-unused-function",
            "-Wno-unused-parameter",
            "-Wno-sign-compare",
        },
    });
    const pcre2_lib = b.addLibrary(.{
        .linkage = .static,
        .name = "pcre2-8",
        .root_module = pcre2_mod,
    });

    // 4. Static library: libstelog_core (shared by CLI, MCP, and tests)
    const core_mod = b.createModule(.{
        .root_source_file = b.path("src/mcp/mcp_schemas.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    core_mod.addIncludePath(b.path("include"));
    core_mod.addIncludePath(b.path("src"));
    core_mod.addIncludePath(b.path("third_party/simdjson"));
    core_mod.addIncludePath(b.path("third_party/simdutf"));
    core_mod.addIncludePath(b.path("third_party/zstd"));
    core_mod.addIncludePath(b.path("third_party/sqlite3"));
    core_mod.addIncludePath(b.path("third_party/pcre2/src"));
    core_mod.linkLibrary(simdjson_lib);
    core_mod.linkLibrary(simdutf_lib);
    core_mod.linkLibrary(zstd_lib);
    core_mod.linkLibrary(sqlite_lib);
    core_mod.linkLibrary(pcre2_lib);

    core_mod.addCSourceFiles(.{
        .files = &.{
            "src/base/arena.c",
            "src/base/hash.c",
            "src/base/time_iso8601.c",
            "src/base/simd.c",
            "src/base/thread_pool.c",
            "src/adapters/ingest.c",
            "src/adapters/codex_adapter.c",
            "src/adapters/agy_adapter.c",
            "src/adapters/pi_adapter.c",
            "src/adapters/droid_adapter.c",
            "src/store/tokenizer.c",
            "src/store/swisstable.c",
            "src/store/roaring.c",
            "src/store/segment_reader.c",
            "src/store/segment_builder.c",
            "src/query/regex_engine.c",
            "src/query/query_engine.c",
            "src/json/json_writer.c",
            "src/mcp/mcp_server.c",
        },
        .flags = c_flags,
    });
    core_mod.addCSourceFile(.{
        .file = b.path("src/json/simdjson_bridge.cpp"),
        .flags = cpp_flags,
    });
    core_mod.addCSourceFile(.{
        .file = b.path("src/base/utf8_bridge.cpp"),
        .flags = cpp_flags,
    });

    const core_lib = b.addLibrary(.{
        .linkage = .static,
        .name = "stelog_core",
        .root_module = core_mod,
    });

    // 5. Executable: stelog
    const exe_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    exe_mod.addIncludePath(b.path("include"));
    exe_mod.addIncludePath(b.path("src"));
    exe_mod.addCSourceFile(.{
        .file = b.path("src/main.c"),
        .flags = c_flags,
    });
    exe_mod.linkLibrary(core_lib);

    const exe = b.addExecutable(.{
        .name = "stelog",
        .root_module = exe_mod,
    });
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run stelog");
    run_step.dependOn(&run_cmd.step);

    // 6. Unit tests: zig build test
    const test_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    test_mod.addIncludePath(b.path("include"));
    test_mod.addIncludePath(b.path("src"));
    test_mod.addCSourceFile(.{
        .file = b.path("tests/test_phase1.c"),
        .flags = c_flags,
    });
    test_mod.linkLibrary(core_lib);

    const test_exe = b.addExecutable(.{
        .name = "test_phase1",
        .root_module = test_mod,
    });
    const run_tests = b.addRunArtifact(test_exe);

    const test2_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    test2_mod.addIncludePath(b.path("include"));
    test2_mod.addIncludePath(b.path("src"));
    test2_mod.addCSourceFile(.{
        .file = b.path("tests/test_phase2.c"),
        .flags = c_flags,
    });
    test2_mod.linkLibrary(core_lib);

    const test2_exe = b.addExecutable(.{
        .name = "test_phase2",
        .root_module = test2_mod,
    });
    const run_tests2 = b.addRunArtifact(test2_exe);

    const test3_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    test3_mod.addIncludePath(b.path("include"));
    test3_mod.addIncludePath(b.path("src"));
    test3_mod.addCSourceFile(.{
        .file = b.path("tests/test_phase3_4.c"),
        .flags = c_flags,
    });
    test3_mod.linkLibrary(core_lib);

    const test3_exe = b.addExecutable(.{
        .name = "test_phase3_4",
        .root_module = test3_mod,
    });
    const run_tests3 = b.addRunArtifact(test3_exe);

    const test_step = b.step("test", "Run all unit tests");
    test_step.dependOn(&run_tests.step);
    test_step.dependOn(&run_tests2.step);
    test_step.dependOn(&run_tests3.step);
}
