"""Exercise installed Windows Files/Plaintext opener registration over a named pipe."""
import os
import struct
import sys
from urllib.parse import parse_qs, quote, urlsplit


def field(data, offset):
    start, size = struct.unpack_from('<II', data, offset)
    assert start <= len(data) and size <= len(data) - start
    return data[start:start + size].decode('utf-8')


def request(pipe, path, body=None):
    method = 'GET' if body is None else 'POST'
    content = body or b''
    with open(pipe, 'r+b', buffering=0) as stream:
        stream.write((f'{method} {path} HTTP/1.1\r\nHost: localhost\r\n'
                      f'Content-Type: application/octet-stream\r\nContent-Length: {len(content)}\r\n\r\n').encode() + content)
        header = b''
        while not header.endswith(b'\r\n\r\n'):
            chunk = stream.read(1)
            assert chunk, 'Incomplete HTTP header'
            header += chunk
        headers = dict(line.lower().split(b': ', 1) for line in header.split(b'\r\n')[1:-2])
        length = int(headers[b'content-length'])
        result = b''
        while len(result) < length:
            chunk = stream.read(length - len(result))
            assert chunk, 'Incomplete HTTP body'
            result += chunk
        return int(header.split()[1]), result


def run(pipe):
    paths = [os.path.join(os.environ['USERPROFILE'], 'Documents', 'opener test.txt'),
             'C:/Users/Test/雪 + # & %.txt', '//server/share/folder/test.txt']
    for path in paths:
        text = path.encode('utf-8')
        binary = struct.pack('<IIII', 0x514f464f, 1, 16, len(text)) + text
        for endpoint, body in [('/api/openers', binary), ('/api/openers?path=' + quote(path, safe=''), None)]:
            status, response = request(pipe, endpoint, body)
            assert status == 200, (status, response)
            assert response[:4] == b'OFOP'
            assert struct.unpack_from('<I', response, 8)[0] == 1, 'Install Plaintext before running this test'
            assert field(response, 40) == 'org.outershell.Plaintext'
            assert field(response, 56).lower().startswith('\\\\.\\pipe\\')
            url = field(response, 64)
            assert parse_qs(urlsplit(url).query)['file'] == [path], url
    for body in [b'', b'bad', struct.pack('<IIII', 0x51465046, 1, 16, 1) + b'x',
                 struct.pack('<IIII', 0x514f464f, 1, 16, 999),
                 struct.pack('<IIII', 0x514f464f, 1, 16, 0)]:
        status, _ = request(pipe, '/api/openers', body)
        assert status == 400, status
    print('PASS Windows native and query openers preserve paths; malformed requests are rejected')


if __name__ == '__main__':
    run(sys.argv[1])
