load("//xla:xla.bzl", "xla_cc_test")
load("@local_tsl//tsl:tsl.default.bzl", "get_compatible_with_portable")
load("@local_tsl//tsl/platform:build_config.bzl", "tf_proto_library")

package_group(
    name = "friends",
    includes = [
        "//xla/python:friends",
    ],
    packages = [
        "//xla/python/...",
    ],
)

package_group(
    name = "internal",
    packages = [
        "//xla/python/ifrt/...",
    ],
)

package(
    default_visibility = ["//visibility:public"],
)

exports_files(
    [
        "BUILD",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "ifrt",
    srcs = [
        "array.cc",
        "client.cc",
        "compiler.cc",
        "device.cc",
        "dtype.cc",
        "executable.cc",
        "future.cc",
        "host_callback.cc",
        "index.cc",
        "index_domain.cc",
        "memory.cc",
        "shape.cc",
        "sharding.cc",
        "tuple.cc",
        "value.cc",
    ],
    hdrs = [
        "array.h",
        "client.h",
        "compiler.h",
        "device.h",
        "dtype.h",
        "executable.h",
        "future.h",
        "host_callback.h",
        "index.h",
        "index_domain.h",
        "memory.h",
        "shape.h",
        "sharding.h",
        "tuple.h",
        "value.h",
    ],
    compatible_with = get_compatible_with_portable(),
    visibility = ["//visibility:public"],
    deps = [
        ":serdes",
        ":types_proto_cc",
        "//xla:status",
        "//xla:statusor",
        "//xla:util",
        "//xla/pjrt:pjrt_client",
        "//xla/pjrt:pjrt_common",
        "//xla/python/ifrt/ir",
        "@com_google_absl//absl/algorithm:container",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:inlined_vector",
        "@com_google_absl//absl/container:node_hash_set",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/types:span",
        "@llvm-project//llvm:Support",
        "@local_tsl//tsl/concurrency:ref_count",
        "@local_tsl//tsl/platform:errors",
        "@local_tsl//tsl/platform:logging",
        "@local_tsl//tsl/platform:statusor",
    ],
)

xla_cc_test(
    name = "array_test",
    size = "small",
    srcs = ["array_test.cc"],
    deps = [
        ":ifrt",
        ":mock",
        "@com_google_googletest//:gtest_main",
        "@llvm-project//llvm:Support",
    ],
)

xla_cc_test(
    name = "future_test",
    size = "small",
    srcs = ["future_test.cc"],
    deps = [
        ":ifrt",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest_main",
        "@local_tsl//tsl/lib/core:status_test_util",
        "@local_tsl//tsl/platform:status_matchers",
    ],
)

xla_cc_test(
    name = "index_domain_test",
    size = "small",
    srcs = ["index_domain_test.cc"],
    deps = [
        ":ifrt",
        "@com_google_googletest//:gtest_main",
    ],
)

xla_cc_test(
    name = "index_test",
    size = "small",
    srcs = ["index_test.cc"],
    deps = [
        ":ifrt",
        "@com_google_googletest//:gtest_main",
    ],
)

xla_cc_test(
    name = "memory_test",
    size = "small",
    srcs = ["memory_test.cc"],
    deps = [
        ":ifrt",
        "@com_google_googletest//:gtest_main",
    ],
)

xla_cc_test(
    name = "shape_test",
    size = "small",
    srcs = ["shape_test.cc"],
    deps = [
        ":ifrt",
        ":types_proto_cc",
        "@com_google_absl//absl/status",
        "@com_google_googletest//:gtest_main",
        "@local_tsl//tsl/platform:status_matchers",
        "@local_tsl//tsl/platform:statusor",
    ],
)

xla_cc_test(
    name = "sharding_test",
    size = "small",
    srcs = ["sharding_test.cc"],
    deps = [
        ":ifrt",
        ":sharding_test_util",
        "//xla/python/ifrt/ir",
        "@com_google_googletest//:gtest_main",
        "@llvm-project//llvm:Support",
        "@local_tsl//tsl/platform:errors",
        "@local_tsl//tsl/platform:status_matchers",
        "@local_tsl//tsl/platform:statusor",
    ],
)

cc_library(
    name = "test_util",
    testonly = True,
    srcs = ["test_util.cc"],
    hdrs = ["test_util.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ifrt",
        "//xla:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/types:span",
        "@local_tsl//tsl/concurrency:ref_count",
        "@local_tsl//tsl/lib/core:status_test_util",
        "@local_tsl//tsl/platform:statusor",
        "@local_tsl//tsl/platform:test",
    ],
)

cc_library(
    name = "sharding_test_util",
    testonly = True,
    srcs = ["sharding_test_util.cc"],
    hdrs = ["sharding_test_util.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ifrt",
        ":mock",
        ":test_util",
        "@local_tsl//tsl/platform:test",
    ],
)

cc_library(
    name = "no_impl_test_main",
    testonly = True,
    srcs = ["no_impl_test_main.cc"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "array_impl_test_lib",
    testonly = True,
    srcs = ["array_impl_test_lib.cc"],
    visibility = ["//visibility:public"],
    deps = [
        ":ifrt",
        ":test_util",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
        "@local_tsl//tsl/lib/core:status_test_util",
        "@local_tsl//tsl/platform:test",
    ],
    alwayslink = True,
)

xla_cc_test(
    name = "array_test_no_impl",
    srcs = [],
    deps = [
        ":array_impl_test_lib",
        ":no_impl_test_main",
    ],
)

cc_library(
    name = "client_impl_test_lib",
    testonly = True,
    srcs = ["client_impl_test_lib.cc"],
    visibility = ["//visibility:public"],
    deps = [
        ":ifrt",
        ":test_util",
        "@local_tsl//tsl/platform:test",
    ],
    alwayslink = True,
)

xla_cc_test(
    name = "client_test_no_impl",
    srcs = [],
    deps = [
        ":client_impl_test_lib",
        ":no_impl_test_main",
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "tuple_impl_test_lib",
    testonly = True,
    srcs = ["tuple_impl_test_lib.cc"],
    visibility = ["//visibility:public"],
    deps = [
        ":ifrt",
        ":test_util",
        "@com_google_absl//absl/types:span",
        "@local_tsl//tsl/concurrency:ref_count",
        "@local_tsl//tsl/lib/core:status_test_util",
        "@local_tsl//tsl/platform:statusor",
        "@local_tsl//tsl/platform:test",
    ],
    alwayslink = True,
)

xla_cc_test(
    name = "tuple_test_no_impl",
    srcs = [],
    deps = [
        ":no_impl_test_main",
        ":tuple_impl_test_lib",
    ],
)

cc_library(
    name = "mock",
    testonly = True,
    srcs = ["mock.cc"],
    hdrs = ["mock.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ifrt",
        "//xla:literal",
        "//xla:test",
        "//xla/pjrt:pjrt_client",
        "//xla/pjrt:pjrt_device_description",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:IR",
        "@local_tsl//tsl/concurrency:ref_count",
    ],
)

cc_library(
    name = "serdes",
    srcs = ["serdes.cc"],
    hdrs = ["serdes.h"],
    compatible_with = get_compatible_with_portable(),
    visibility = ["//visibility:public"],
    deps = [
        ":serdes_proto_cc",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@llvm-project//llvm:Support",
        "@local_tsl//tsl/platform:statusor",
    ],
)

xla_cc_test(
    name = "serdes_test",
    srcs = ["serdes_test.cc"],
    deps = [
        ":serdes",
        ":serdes_proto_cc",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
        "@llvm-project//llvm:Support",
        "@local_tsl//tsl/platform:errors",
        "@local_tsl//tsl/platform:status_matchers",
        "@local_tsl//tsl/platform:statusor",
    ],
)

tf_proto_library(
    name = "serdes_proto",
    srcs = ["serdes.proto"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "sharding_serdes",
    srcs = ["sharding_serdes.cc"],
    hdrs = ["sharding_serdes.h"],
    compatible_with = get_compatible_with_portable(),
    visibility = ["//visibility:public"],
    deps = [
        ":ifrt",
        ":serdes",
        ":sharding_proto_cc",
        "//xla:statusor",
        "@com_google_absl//absl/status",
        "@llvm-project//llvm:Support",
        "@local_tsl//tsl/platform:statusor",
    ],
    alwayslink = True,
)

xla_cc_test(
    name = "sharding_serdes_test",
    srcs = ["sharding_serdes_test.cc"],
    deps = [
        ":ifrt",
        ":serdes",
        ":serdes_proto_cc",
        ":sharding_serdes",
        ":sharding_test_util",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_googletest//:gtest_main",
        "@local_tsl//tsl/platform:statusor",
    ],
)

tf_proto_library(
    name = "types_proto",
    srcs = ["types.proto"],
    visibility = ["//visibility:public"],
)

tf_proto_library(
    name = "sharding_proto",
    srcs = ["sharding.proto"],
    protodeps = [":types_proto"],
    visibility = ["//visibility:public"],
)
