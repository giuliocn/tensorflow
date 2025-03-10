load("@llvm-project//mlir:tblgen.bzl", "gentbl_cc_library")
load("//tensorflow:tensorflow.bzl", "if_google", "tf_cc_test")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = [":friends"],
    licenses = ["notice"],
)

package_group(
    name = "friends",
    packages = [
        "//tensorflow/compiler/mlir/tfrt/...",
        "//tensorflow/core/tfrt/ifrt/...",
        "//tensorflow/core/tfrt/saved_model/tests/...",
    ] + if_google([
        "//learning/brain/tfrt/cpp_tests/...",
        # Allow visibility from the mlir language server.
        "//learning/brain/mlir/mlir_lsp_server/...",
    ]),
)

gentbl_cc_library(
    name = "pass_inc_gen",
    tbl_outs = [
        (
            [
                "-gen-pass-decls",
                "-name=TfrtIfrtServing",
            ],
            "passes.h.inc",
        ),
    ],
    tblgen = "@llvm-project//mlir:mlir-tblgen",
    td_file = "passes.td",
    deps = [
        "@llvm-project//mlir:PassBaseTdFiles",
    ],
)

cc_library(
    name = "ifrt_constants",
    srcs = [],
    hdrs = ["ifrt_constants.h"],
    deps = [
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "tf_ifrt_passes",
    srcs = [
        "rewrite_cluster_to_ifrt_call.cc",
        "tf_ifrt_passes.cc",
    ],
    hdrs = [
        "tf_ifrt_passes.h",
    ],
    textual_hdrs = ["passes.h.inc"],
    deps = [
        ":ifrt_constants",
        ":pass_inc_gen",
        "//tensorflow/compiler/mlir/tensorflow",
        "//tensorflow/compiler/mlir/tensorflow:bridge_logger",
        "//tensorflow/compiler/mlir/tensorflow:device_util",
        "//tensorflow/compiler/mlir/tensorflow:dump_mlir_util",
        "//tensorflow/compiler/mlir/tensorflow:error_util",
        "//tensorflow/compiler/mlir/tensorflow:tensorflow_ops",
        "//tensorflow/compiler/mlir/tensorflow:tpu_rewrite_device_util",
        "//tensorflow/compiler/mlir/tensorflow/transforms:tensorflow_passes",
        "//tensorflow/compiler/mlir/tensorflow/transforms/host_runtime:tpu_metadata_utils",
        "//tensorflow/core:framework",
        "//tensorflow/core/platform:random",
        "//tensorflow/core/protobuf/tpu:compile_metadata_proto_cc",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:FuncDialect",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Pass",
        "@llvm-project//mlir:Support",
        "@local_tsl//tsl/platform:protobuf",
    ],
)

cc_library(
    name = "tf2hlo",
    srcs = ["tf2hlo.cc"],
    hdrs = ["tf2hlo.h"],
    deps = [
        ":ifrt_constants",
        "//tensorflow/compiler/jit:xla_cpu_jit",
        "//tensorflow/compiler/mlir/tensorflow:dump_mlir_util",
        "//tensorflow/compiler/mlir/tensorflow:serialize_mlir_module_utils",
        "//tensorflow/compiler/mlir/tf2xla/api/v2:legalize_tf",
        "//tensorflow/compiler/tf2xla:layout_util",
        "//tensorflow/compiler/tf2xla:xla_compiler",
        "//tensorflow/core:core_cpu_base",
        "//tensorflow/core:framework",
        "//tensorflow/core:lib_headers_for_pybind",
        "//tensorflow/core:protos_all_cc",
        "//tensorflow/core/protobuf/tpu:compile_metadata_proto_cc",
        "//tensorflow/core/protobuf/tpu:topology_proto_cc",
        "//tensorflow/core/tpu/kernels:tpu_compile_op_support",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:FuncDialect",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Pass",
        "@local_tsl//tsl/platform:errors",
        "@local_tsl//tsl/platform:protobuf",
        "@local_tsl//tsl/platform:statusor",
        "@local_xla//xla:shape_util",
        "@local_xla//xla:xla_data_proto_cc",
        "@local_xla//xla/client:client_library",
        "@local_xla//xla/python/ifrt",
        "@local_xla//xla/service:computation_placer_hdr",
        "@local_xla//xla/service/llvm_ir:llvm_util",
        "@local_xla//xla/stream_executor",
        "@local_xla//xla/translate/hlo_to_mhlo:hlo_to_mlir_hlo",
    ],
)

tf_cc_test(
    name = "tf2hlo_test",
    srcs = [
        "tf2hlo_test.cc",
    ],
    data = [
        "//tensorflow/compiler/mlir/tfrt/transforms/ifrt/testdata",
    ],
    tags = ["no_oss"],
    deps = [
        ":tf2hlo",
        "//tensorflow/compiler/mlir/tensorflow",
        "//tensorflow/compiler/tf2xla:xla_helpers",
        "//tensorflow/core:framework",
        "//tensorflow/core:test",
        "//tensorflow/core/platform:resource_loader",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
        "@llvm-project//mlir:AllPassesAndDialects",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Parser",
        "@local_tsl//tsl/platform:protobuf",
        "@local_tsl//tsl/platform:statusor",
        "@local_xla//xla/python/ifrt",
        "@local_xla//xla/python/ifrt:test_util",
        "@local_xla//xla/python/pjrt_ifrt:tfrt_cpu_client_test_lib",
    ],
)

cc_library(
    name = "ifrt_backend_compiler",
    srcs = ["ifrt_backend_compiler.cc"],
    hdrs = ["ifrt_backend_compiler.h"],
    deps = [
        ":tf_ifrt_passes",
        "//tensorflow/compiler/mlir/tensorflow:dump_mlir_util",
        "//tensorflow/compiler/mlir/tensorflow:error_util",
        "//tensorflow/compiler/mlir/tensorflow:visitor",
        "//tensorflow/compiler/mlir/tf2xla/api/v2:cluster_tf",
        "//tensorflow/compiler/mlir/tfrt:backend_compiler",
        "//tensorflow/compiler/mlir/tfrt:tpu_passes",
        "//tensorflow/core/tfrt/ifrt:ifrt_executable_registry",
        "//tensorflow/core/tfrt/ifrt:ifrt_model_context",
        "//tensorflow/core/tfrt/ifrt:ifrt_serving_executable",
        "//tensorflow/core/tfrt/runtime",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@llvm-project//llvm:Support",
        "@llvm-project//mlir:FuncDialect",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Support",
        "@local_tsl//tsl/platform:errors",
        "@local_tsl//tsl/platform:statusor",
        "@local_tsl//tsl/profiler/lib:traceme",
    ],
)

tf_cc_test(
    name = "ifrt_backend_compiler_test",
    srcs = [
        "ifrt_backend_compiler_test.cc",
    ],
    data = [
        "//tensorflow/compiler/mlir/tfrt/transforms/ifrt/testdata",
    ],
    tags = ["no_oss"],
    deps = [
        ":ifrt_backend_compiler",
        "//tensorflow/compiler/mlir/tensorflow",
        "//tensorflow/core:test",
        "//tensorflow/core/platform:resource_loader",
        "//tensorflow/core/tfrt/graph_executor:graph_execution_options",
        "//tensorflow/core/tfrt/ifrt:ifrt_model_context",
        "//tensorflow/core/tfrt/runtime",
        "//tensorflow/core/tfrt/saved_model:saved_model_testutil",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
        "@llvm-project//mlir:AllPassesAndDialects",
        "@llvm-project//mlir:IR",
        "@llvm-project//mlir:Parser",
        "@local_tsl//tsl/platform:statusor",
        "@local_xla//xla/python/ifrt",
        "@local_xla//xla/python/ifrt:test_util",
        "@local_xla//xla/python/pjrt_ifrt:tfrt_cpu_client_test_lib",
        "@tf_runtime//:hostcontext",
    ],
)
