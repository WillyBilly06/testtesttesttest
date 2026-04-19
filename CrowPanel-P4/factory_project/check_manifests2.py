import sys, os, traceback
from idf_component_tools.manifest.models import Manifest
from ruamel.yaml import YAML

yaml = YAML()

project_dir = r'D:\ESP32-Code\CrowPanel-P4\factory_project'

dirs_to_check = []
for dp, dn, fn in os.walk(project_dir):
    if 'idf_component.yml' in fn:
        rel = os.path.relpath(dp, project_dir)
        dirs_to_check.append((rel, dp))

for name, d in dirs_to_check:
    yml = os.path.join(d, 'idf_component.yml')
    print(f'\nChecking: {name} ({yml})')
    try:
        with open(yml) as f:
            data = yaml.load(f)
        if data is None:
            data = {}
        print(f'  Raw data keys: {list(data.keys()) if data else "empty"}')
        # Try to validate with the Manifest model
        m = Manifest(**data)
        print(f'  Manifest OK, name={m.name!r}')
    except Exception as e:
        print(f'  ERROR: {e}')
