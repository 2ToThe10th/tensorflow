licenses(["notice"])  # Apache 2.0

package(default_visibility = ["//visibility:public"])

alias(name="LICENSE", actual = "%{target}:LICENSE")
alias(name = "tensorrt_headers", actual = "%{target}:tensorrt_headers")
alias(name = "nv_infer", actual = "%{target}:nv_infer")
