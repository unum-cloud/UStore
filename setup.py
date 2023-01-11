import os
import re
import subprocess
import sys
from distutils.dir_util import copy_tree
import time
import multiprocessing
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext

__version__ = open('VERSION', 'r').read()
__libname__ = 'ukv'


this_directory = os.path.abspath(os.path.dirname(__file__))
with open(os.path.join(this_directory, 'README.md')) as f:
    long_description = f.read()


class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=''):
        Extension.__init__(self, name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)


class CMakeBuild(build_ext):
    def build_extension(self, ext):
        self.parallel = multiprocessing.cpu_count() // 2
        extdir = os.path.abspath(os.path.dirname(
            self.get_ext_fullpath(ext.name)))

        # required for auto-detection & inclusion of auxiliary "native" libs
        if not extdir.endswith(os.path.sep):
            extdir += os.path.sep

        if 'PYTHON_DEBUG' in os.environ:
            os.makedirs(extdir, exist_ok=True)
            copy_tree("./build/lib/", extdir)
            return

        cmake_args = [
            f'-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extdir}',
            f'-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={extdir}',
            f'-DPYTHON_EXECUTABLE={sys.executable}',
            '-DUKV_BUILD_ENGINE_UMEM=1',
            '-DUKV_BUILD_ENGINE_LEVELDB=1',
            '-DUKV_BUILD_ENGINE_ROCKSDB=1',
            '-DUKV_BUILD_API_FLIGHT_CLIENT=1',
            '-DUKV_BUILD_SDK_PYTHON=1'
        ]

        # Adding CMake arguments set as environment variable
        # (needed e.g. to build for ARM OSx on conda-forge)
        if 'CMAKE_ARGS' in os.environ:
            cmake_args += [
                item for item in os.environ['CMAKE_ARGS'].split(' ') if item]
        elif 'CMAKE_ARGS_F' in os.environ:
            cmake_args += [item.strip()
                           for item in open(os.environ['CMAKE_ARGS_F']).read().split(' ') if item]

        if sys.platform.startswith('darwin'):
            # Cross-compile support for macOS - respect ARCHFLAGS if set
            archs = re.findall(r'-arch (\S+)', os.environ.get('ARCHFLAGS', ''))
            if archs:
                cmake_args += [
                    '-DCMAKE_OSX_ARCHITECTURES={}'.format(';'.join(archs))]

        # Set CMAKE_BUILD_PARALLEL_LEVEL to control the parallel build level
        # across all generators.
        build_args = []
        if 'CMAKE_BUILD_PARALLEL_LEVEL' not in os.environ:
            # self.parallel is a Python 3 only way to set parallel jobs by hand
            # using -j in the build_ext call, not supported by pip or PyPA-build.
            if hasattr(self, 'parallel') and self.parallel:
                build_args += [f'-j{self.parallel}']

        subprocess.check_call(['cmake', ext.sourcedir] + cmake_args)
        subprocess.check_call(
            ['cmake', '--build', '.', '--target', ext.name.replace('ukv.', 'py_')] + build_args)


setup(
    name=__libname__,
    version=__version__,

    author='Ashot Vardanian',
    author_email='info@unum.cloud',
    url='https://github.com/unum-cloud/ukv',
    description='Python bindings for Unum\'s Universal Key-Value store.',
    long_description=long_description,
    long_description_content_type='text/markdown',
    classifiers=[
        'Development Status :: 4 - Beta',
        'Intended Audience :: Developers',
        'Intended Audience :: Information Technology',
        'License :: OSI Approved :: Apache Software License 2.0 (Apache-2.0)',
        'Natural Language :: English',
        'Operating System :: POSIX',
        'Operating System :: POSIX :: Linux',
        'Programming Language :: C',
        'Programming Language :: C++',
        'Programming Language :: Python :: 3 :: Only',
        'Programming Language :: Python :: Implementation :: CPython',
        'Topic :: Database',
        # More classifiers to come later:
        # https://pypi.org/classifiers/
        # 'Environment :: GPU',
        # 'Framework :: Apache Airflow :: Provider',
        # 'Framework :: IPython',
    ],
    ext_modules=[
        CMakeExtension('ukv.umem'),
        CMakeExtension('ukv.rocksdb'),
        CMakeExtension('ukv.leveldb'),
        CMakeExtension('ukv.flight_client')],
    cmdclass={'build_ext': CMakeBuild},
    zip_safe=False,
    install_requires=[
        'numpy>=1.16',
        'pyarrow>=10.0.0,<11'
    ],
    extras_require={'test': 'pytest'},
    python_requires='>=3.7',
)
