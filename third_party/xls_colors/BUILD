# XLS color library ported from https://www.vagrearg.org/content/hsvrgb

load(
    "//xls/build_rules:xls_build_defs.bzl",
    "xls_dslx_module_library",
    "xls_dslx_test",
)

package(
    default_visibility = ["//xls:xls_internal"],
)

licenses(["notice"])

exports_files(["LICENSE"])

xls_dslx_module_library(
    name = "hsv2rgb_dslx_module",
    src = "hsv2rgb.x",
)

xls_dslx_test(
    name = "hsv2rgb_dslx_test",
    dep = ":hsv2rgb_dslx_module",
)

cc_test(
    name = "hsv2rgb_verilog_test",
    srcs = ["test.cc"],
    data = [
        ":hsv2rgb_dslx_module",
    ],
    shard_count = 6,
    tags = ["optonly"],
    deps = [
        ":fast_hsv2rgb",
        "@com_google_googletest//:gtest",
        "//xls/codegen:combinational_generator",
        "//xls/common:xls_gunit_main",
        "//xls/common/file:filesystem",
        "//xls/common/file:get_runfile_path",
        "//xls/common/status:matchers",
        "//xls/dslx:ir_converter",
        "//xls/dslx:parse_and_typecheck",
        "//xls/ir",
        "//xls/ir:ir_parser",
        "//xls/ir:ir_test_base",
    ],
)

cc_library(
    name = "fast_hsv2rgb",
    srcs = ["fast_hsv2rgb_32bit.c"],
    hdrs = ["fast_hsv2rgb.h"],
)
