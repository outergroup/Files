const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const source = fs.readFileSync(require('node:path').join(__dirname, '../Resources/FilesWeb/app.js'), 'utf8');
let url;
const context = { URLSearchParams, state: { target: 'docker:abc', path: '' }, history: { replaceState(_a, _b, value) { url = value; } }, updateEnvironmentName() {}, document: {}, $() { return { textContent: 'Host' }; } };
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('function updateLocation()'), source.indexOf('function switchEnvironment()')), context);
for (const path of ['/Users/name/folder/file.txt', '/tmp/a & b+雪/#file?%2F.txt', '~']) {
    context.state.path = path;
    context.updateLocation();
    assert(!/%2f/i.test(url.replace(/%252F/gi, '')));
    assert.equal(new URLSearchParams(url.slice(1)).get('path'), path);
    assert.equal(new URLSearchParams(url.slice(1)).get('target'), 'docker:abc');
}
console.log('PASS readable URL slashes and reserved-character round trips');
