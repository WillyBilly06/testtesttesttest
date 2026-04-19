import os, yaml
root = '.'
for dp, dn, fn in os.walk(root):
    for f in fn:
        if f == 'idf_component.yml':
            path = os.path.join(dp, f)
            try:
                with open(path) as fh:
                    data = yaml.safe_load(fh)
                if data and 'name' in data:
                    n = data['name']
                    print(f'HAS NAME: {path} => name={n!r} type={type(n).__name__}')
            except Exception as e:
                print(f'ERROR: {path} => {e}')
print('Done scanning.')
