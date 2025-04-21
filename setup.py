from setuptools import Extension, setup

# assumes gcc-compatible command line options
WARNING_OPTIONS = [
    "-std=gnu11",
    "-Wall",
    "-Wextra",
    "-Wconversion",
    "-Wpedantic",
    "-Wmissing-prototypes",
    "-Wstrict-prototypes",
    "-Wno-unused-parameter",
    "-Wno-missing-field-initializers",
    "-Wno-cast-function-type",
    "-Werror",
]

setup(ext_modules=[
    Extension(
        "ctrlc.interruptible",
        sources = [
            "ctrlc/interruptible.c",
            "ctrlc/kissfft_subset.c",
        ],
        extra_compile_args = WARNING_OPTIONS,
    ),
    Extension(
        "ctrlc.signaler",
        sources = [
            "ctrlc/signaler.c",
        ],
        extra_compile_args = WARNING_OPTIONS,
    ),
])
