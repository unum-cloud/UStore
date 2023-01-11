import os

from conans import ConanFile
from conans.tools import SystemPackageTool
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout


class ConanUKV(ConanFile):

    exports = 'VERSION', 'LICENSE', 'README.md'
    exports_sources = 'CMakeLists.txt', 'src/*', 'include/*', 'cmake/*', 'VERSION'
    options = {'with_arrow': [True, False]}
    default_options = {
        'with_arrow': False,
    }

    name = 'UKV'
    version = open('VERSION').read()
    # Take just the first line
    license = open('LICENSE').read().split('\n', 1)[0]
    description = open('README.md').read()
    url = 'https://github.com/unum-cloud/ukv.git'
    homepage = 'https://unum.cloud/ukv'

    # Complete list of possible settings:
    # https://docs.conan.io/en/latest/extending/custom_settings.html
    settings = {
        # https://docs.conan.io/en/latest/introduction.html#all-platforms-all-build-systems-and-compilers
        'os': ['Linux'],
        # https://docs.conan.io/en/latest/integrations/compilers.html
        'compiler': [
            'gcc', 'clang', 'intel',
            # Not fully supported yet:
            # 'intel-cc'
        ],
        # https://github.com/conan-io/conan/issues/2500
        'build_type': ['Release'],
        'arch': ['x86', 'x86_64', 'armv8', 'armv8_32', 'armv8.3'],
    }
    generators = 'CMakeToolchain',

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()

    def build(self):
        # https://docs.conan.io/en/1.50/reference/build_helpers/cmake.html#constructor
        cmake = CMake(self)
        cmake.configure(variables={
            'UKV_BUILD_SDK_PYTHON': 0,
            'UKV_BUILD_TESTS': 0,
            'UKV_BUILD_BENCHMARKS': 0,
            'UKV_BUILD_API_FLIGHT_CLIENT': self.options['with_arrow'],
        })
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

        self.copy('*.a', dst='lib',
                  src=os.path.join(self.source_folder, 'build/lib'))

    def package_info(self):
        # Larger projects like UKV or Boost would have a lot of parts,
        # but it is recommended to ship one library per component
        # https://docs.conan.io/en/1.26/creating_packages/package_information.html#using-components
        self.cpp_info.name = 'ukv'
        self.cpp_info.includedirs = ['include/']
        self.cpp_info.components['umem'].libs = ['libukv_embedded_umem']
        self.cpp_info.components['leveldb'].libs = ['libukv_embedded_leveldb']
        self.cpp_info.components['rocksdb'].libs = ['libukv_embedded_rocksdb']
        if self.options['with_arrow']:
            self.cpp_info.components['flight_client'].libs = [
                'ukv_flight_client']

    def requirements(self):
        # Most of our dependencies come from `CMake`
        # https://docs.conan.io/en/latest/reference/conanfile/methods.html#requirements

        if self.options['with_arrow']:
            # https://conan.io/center/arrow
            self.requires('arrow/8.0.1')

            # We are overriding this to avoid collisions within Arrow libraries
            self.requires('protobuf/3.21.4')

    def system_requirements(self):
        # If we need a fresher version of Arrow, we can get it from the default package manager.
        # https://docs.conan.io/en/latest/reference/conanfile/methods.html?#system-requirements

        # https://arrow.apache.org/install/
        # SystemPackageTool().install('libarrow-dev ')
        # SystemPackageTool().install('libarrow-flight-dev')
        pass
