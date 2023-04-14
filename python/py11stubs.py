
import importlib.util
import json
from os.path import exists, dirname, join
import sys

import pybind11_stubgen


class _PackageFinder:
    """
    Custom loader to allow loading built modules from their location
    in the build directory(as opposed to their install location)
    """

    mapping = {}

    @classmethod
    def find_spec(cls, fullname, path, target=None):
        m = cls.mapping.get(fullname)
        if m:
            return importlib.util.spec_from_file_location(fullname, m)


def generate_pyi(module_name: str, pyi_filename: str) -> bool:

    pybind11_stubgen.FunctionSignature.n_invalid_signatures = 0
    module = pybind11_stubgen.ModuleStubsGenerator(module_name)
    module.parse()
    if pybind11_stubgen.FunctionSignature.n_invalid_signatures > 0:
        print('FAILED to generate pyi for', module_name, file=sys.stderr)
        return False

    module.write_setup_py = False
    with open(pyi_filename, 'w') as fp:
        fp.write('#\n# AUTOMATICALLY GENERATED FILE, DO NOT EDIT!\n#\n\n')
        fp.write('\n'.join(module.to_lines()))

    typed = join(dirname(pyi_filename), 'py.typed')

    if not exists(typed):
        with open(typed, 'w') as fp:
            pass

    return True


def main():
    cfg = json.loads(sys.argv[1])
    _PackageFinder.mapping = cfg['mapping']
    sys.meta_path.insert(0, _PackageFinder)

    # Generate pyi modules
    sys.argv = [
        '<dummy>',
        '--no-setup-py',
        '--log-level=WARNING',
        '--root-module-suffix=',
        '--ignore-invalid',
        'all',
        '-o',
        cfg['out'],
    ] + cfg['stubs']

    pybind11_stubgen.main()


if __name__ == '__main__':
    main()
