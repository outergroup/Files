#!/usr/bin/env python3
import os
from pathlib import Path
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'Helper'))
from client_binary import run_client

with tempfile.TemporaryDirectory(prefix='files-favorites-') as temporary:
    work = Path(temporary).resolve()
    env = dict(os.environ, XDG_CONFIG_HOME=str(work / 'config'))
    folder = work / 'Favorite ☃ folder'
    folder.mkdir()
    alias = work / 'alias'
    alias.symlink_to(folder)
    config = work / 'config/outer-files/places.tsv'
    def call(operation, **values):
        return run_client(sys.argv[1], operation, env=env, **values)
    assert call('addPlace', path=str(folder))['ok']
    assert config.read_text() == f'{folder}\t{folder.name}\n'
    assert call('list', path=str(alias))['path'] == str(folder)
    assert call('addPlace', path=str(alias))['ok']
    assert len(config.read_text().splitlines()) == 1
    places = call('places')['entries']
    assert any(p['path'] == str(folder) and p['name'] == folder.name for p in places)
    file = work / 'file.txt'
    file.write_text('test')
    invalid = work / 'tab\tfolder'
    invalid.mkdir()
    for path in [file, invalid, work / 'missing']:
        assert 'error' in call('addPlace', path=str(path))
    assert 'error' in call('addPlace', target='docker-user:abc', path=str(folder))
    config.write_text(config.read_text().rstrip('\n'))
    other = work / 'Other'
    other.mkdir()
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda _: call('addPlace', path=str(other)), range(8)))
    assert all(result.get('ok') for result in results)
    assert config.read_text().splitlines() == [f'{folder}\t{folder.name}', f'{other}\tOther']
    places = call('places')['entries']
    assert all(p['mode'] == 'favorite' for p in places if p['path'] in (str(folder), str(other)))
    assert all(p['mode'] == '' for p in places if p['name'] == 'Home')
    assert 'error' in call('removePlace', target='docker-user:abc', path=str(folder))
    assert call('removePlace', path=str(alias))['ok']
    assert config.read_text().splitlines() == [f'{other}\tOther']
    assert 'error' in call('removePlace', path=str(folder))
    other.rmdir()
    assert call('removePlace', path=str(other))['ok']
    assert config.read_text() == ''
    assert call('addPlace', path=str(folder))['ok']
    assert config.read_text() == f'{folder}\t{folder.name}\n'
    builtin = next((p for p in call('places')['entries'] if p['mode'] == 'place'), None)
    if builtin:
        assert call('removePlace', path=builtin['path'])['ok']
        assert f"!{builtin['path']}" in config.read_text().splitlines()
        assert all(p['path'] != builtin['path'] for p in call('places')['entries'])
        assert call('addPlace', path=builtin['path'])['ok']
        assert f"!{builtin['path']}" not in config.read_text().splitlines()
        assert any(p['path'] == builtin['path'] and p['mode'] == 'favorite' for p in call('places')['entries'])
        assert call('removePlace', path=builtin['path'])['ok']
        assert all(p['path'] != builtin['path'] for p in call('places')['entries'])
        assert config.read_text().splitlines() == [f'{folder}\t{folder.name}', f"!{builtin['path']}"]
    home = next(p for p in call('places')['entries'] if p['name'] == 'Home')
    assert 'error' in call('removePlace', path=home['path'])
    env['XDG_CONFIG_HOME'] = str(file)
    assert 'error' in call('addPlace', path=str(folder))
    assert 'error' in call('removePlace', path=str(folder))
print('PASS favorites persistence across processes, canonical duplicates, concurrent writes, TSV validation, removal, hiding built-in places, host scope and write errors' + ('' if builtin else ' (no built-in places to hide here)'))
