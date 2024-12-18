from setuptools import Extension, setup

setup(ext_modules=[
    Extension(
        "ctrlc.interruptible",
        sources = [
            "ctrlc/interruptible.c",
            "ctrlc/kissfft_subset.c",
        ],
    ),
    Extension(
        "ctrlc.signaler",
        sources = [
            "ctrlc/signaler.c",
        ],
    ),
])
