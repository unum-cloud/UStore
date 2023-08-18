import os
import re
import sys
import json
from distutils.dir_util import copy_tree
from os.path import abspath, exists, dirname, join
import multiprocessing
from setuptools import setup, Extension, Command
import subprocess
import multiprocessing
from distutils.dir_util import copy_tree
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext

__version__ = open("VERSION", "r").read().strip()
__lib_name__ = "ustore"


this_directory = os.path.abspath(dirname(__file__))
with open(os.path.join(this_directory, "README.md")) as f:
    long_description = f.read()


class CMakeExtension(Extension):
    def __init__(self, name, source_dir=""):
        Extension.__init__(self, name, sources=[])
        self.source_dir = os.path.abspath(source_dir)


class CMakeBuild(build_ext):
    def build_extension(self, ext):
        self.parallel = multiprocessing.cpu_count() // 2
        extension_dir = os.path.abspath(dirname(self.get_ext_fullpath(ext.name)))

        # required for auto-detection & inclusion of auxiliary 'native' libs
        if not extension_dir.endswith(os.path.sep):
            extension_dir += os.path.sep

        if "USTORE_DEBUG_PYTHON" in os.environ:
            os.makedirs(extension_dir, exist_ok=True)
            copy_tree("./build/lib/", extension_dir)
            return

        cmake_args = [
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extension_dir}",
            f"-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={extension_dir}",
            f"-DPYTHON_EXECUTABLE={sys.executable}",
            "-DUSTORE_BUILD_ENGINE_UCSET=1",
            "-DUSTORE_BUILD_ENGINE_LEVELDB=1",
            "-DUSTORE_BUILD_ENGINE_ROCKSDB=1",
            "-DUSTORE_BUILD_API_FLIGHT_CLIENT=1",
            "-DUSTORE_BUILD_SDK_PYTHON=1",
            "-DUSTORE_BUILD_TESTS=0",
            "-DUSTORE_USE_PYARROW=1",
            "-DUSE_CONAN=1",
        ]

        # Adding CMake arguments set as environment variable
        # (needed e.g. to build for ARM OSx on conda-forge)
        if "CMAKE_ARGS" in os.environ:
            cmake_args += [item for item in os.environ["CMAKE_ARGS"].split(" ") if item]
        elif "CMAKE_ARGS_F" in os.environ:
            cmake_args += [
                item.strip()
                for item in open(os.environ["CMAKE_ARGS_F"]).read().split(" ")
                if item
            ]

        if sys.platform.startswith("darwin"):
            # Cross-compile support for macOS - respect ARCHFLAGS if set
            archs = re.findall(r"-arch (\S+)", os.environ.get("ARCHFLAGS", ""))
            if archs:
                cmake_args += ["-DCMAKE_OSX_ARCHITECTURES={}".format(";".join(archs))]

        # Set CMAKE_BUILD_PARALLEL_LEVEL to control the parallel build level
        # across all generators.
        build_args = []
        if "CMAKE_BUILD_PARALLEL_LEVEL" not in os.environ:
            # self.parallel is a Python 3 only way to set parallel jobs by hand
            # using -j in the build_ext call, not supported by pip or PyPA-build.
            if hasattr(self, "parallel") and self.parallel:
                build_args += [f"-j{self.parallel}"]

        subprocess.check_call(["cmake", ext.source_dir] + cmake_args)
        subprocess.check_call(
            ["cmake", "--build", ".", "--target", ext.name.replace("ustore.", "py_")]
            + build_args
        )

    def run(self):
        build_ext.run(self)
        if "USTORE_DEBUG_PYTHON" not in os.environ:
            self.run_command("build_pyi")


class BuildPyi(Command):
    command_name = "build_pyi"
    description = "Generates pyi files from built extensions"

    def initialize_options(self):
        self.build_lib = None

    def finalize_options(self):
        self.set_undefined_options("build", ("build_lib", "build_lib"))

    def run(self):
        # Gather information for needed stubs
        data = {"mapping": {}, "stubs": []}
        env = os.environ.copy()

        # Requires information from build_ext to work
        build_ext = self.distribution.get_command_obj("build_ext")
        if build_ext.inplace:
            inst_command = self.distribution.get_command_obj("install")
            inst_command.ensure_finalized()
            data["out"] = inst_command.install_platlib
        else:
            data["out"] = self.build_lib

        wrappers = ["ustore"]
        # Ensure that the associated packages can always be found locally
        for wrapper in wrappers:
            pkgdir = wrapper.split(".")
            init_py = abspath(join(self.build_lib, *pkgdir, "__init__.py"))
            data["mapping"][wrapper] = init_py
            if not exists(init_py):
                open(init_py, "w").close()

        for ext in build_ext.extensions:
            fname = build_ext.get_ext_filename(ext.name)
            data["mapping"][ext.name] = abspath(join(self.build_lib, fname))
            data["stubs"].append(ext.name)

        data_json = json.dumps(data)
        # Execute in a subprocess in case it crashes
        args = [sys.executable, "-m", "python.py11stubs", data_json]

        proc = subprocess.run(args, env=env, capture_output=True)
        print("exit status:", proc.returncode)
        print("stdout:", proc.stdout.decode())
        print("stderr:", proc.stderr.decode())
        if proc.returncode != 0:
            raise RuntimeError(
                "Failed to generate .pyi file via %s" % (args,)
            ) from None

        # Create a py.typed for PEP 561
        with open(join(data["out"], "ustore", "py.typed"), "w"):
            pass


setup(
    name=__lib_name__,
    version=__version__,
    packages=["ustore"],
    package_dir={"ustore": "python/ustore"},
    author="Ash Vardanian",
    author_email="info@unum.cloud",
    url="https://github.com/unum-cloud/ustore",
    description="Python bindings for Unum's UStore.",
    long_description=long_description,
    long_description_content_type="text/markdown",
    license="Apache-2.0",
    classifiers=[
        "Development Status :: 5 - Production/Stable",
        "Natural Language :: English",
        "Intended Audience :: Developers",
        "Intended Audience :: Information Technology",
        "License :: OSI Approved :: Apache Software License",
        "Operating System :: Unix",
        "Operating System :: POSIX",
        "Operating System :: POSIX :: Linux",
        "Programming Language :: C",
        "Programming Language :: C++",
        "Programming Language :: Python :: Implementation :: CPython",
        "Programming Language :: Python :: 3 :: Only",
        "Programming Language :: Python :: 3.7",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Topic :: Database",
        "Topic :: Database :: Database Engines/Servers",
        "Topic :: Database :: Front-Ends",
    ],
    ext_modules=[
        CMakeExtension("ustore.ucset"),
        CMakeExtension("ustore.rocksdb"),
        CMakeExtension("ustore.leveldb"),
        CMakeExtension("ustore.flight_client"),
    ],
    cmdclass={
        "build_ext": CMakeBuild,
        "build_pyi": BuildPyi,
    },
    zip_safe=False,
    install_requires=["numpy>=1.16", "pyarrow>=10.0.1,<11"],
    extras_require={"test": "pytest"},
    python_requires=">=3.7",
)
