import sys, os, traceback
sys.path.insert(0, r'C:\Users\ngocv\.espressif\python_env\idf5.5_py3.11_env\Lib\site-packages')

from idf_component_manager.core import ManifestManager
from idf_component_tools.manifest_validator import ManifestValidator

# Try validating each component manifest
components_dir = r'D:\ESP32-Code\CrowPanel-P4\factory_project\components'
main_dir = r'D:\ESP32-Code\CrowPanel-P4\factory_project\main'

dirs_to_check = [main_dir]
for d in os.listdir(components_dir):
    full = os.path.join(components_dir, d)
    if os.path.isdir(full):
        yml = os.path.join(full, 'idf_component.yml')
        if os.path.exists(yml):
            dirs_to_check.append(full)

for d in dirs_to_check:
    yml = os.path.join(d, 'idf_component.yml')
    print(f'\nChecking: {yml}')
    try:
        v = ManifestValidator(yml, d)
        errors = v.validate_normalize()
        if errors:
            print(f'  ERRORS: {errors}')
        else:
            print(f'  OK')
    except Exception as e:
        traceback.print_exc()
        print(f'  EXCEPTION: {e}')
