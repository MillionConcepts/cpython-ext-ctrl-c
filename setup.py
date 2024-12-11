from setuptools import Extension, setup

setup(ext_modules=[
    Extension(
        "ctrlc.interruptible",
        sources = [
            "ctrlc/interruptible.c",
            # ...
        ],
    ),
    Extension(
        "ctrlc.signaler",
        sources = [
            "ctrlc/signaler.c",
        ],
    ),
])
