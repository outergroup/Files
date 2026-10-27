#!/usr/bin/env python3
"""Exercise the HTTP binary boundary, including the guard as the final header."""
import http.client
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'Helper'))
from client_binary import encode_client, decode_client
backend=sys.argv[1]
app='Files'
class Connection(http.client.HTTPConnection):
 def __init__(self,path):super().__init__('localhost',timeout=10);self.path=path
 def connect(self):self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(10);self.sock.connect(self.path)
with tempfile.TemporaryDirectory(prefix='env-http-',dir='/tmp') as temp:
 work=Path(temp);sock=work/'app.sock';web=work/'web';web.mkdir()
 (web/'environment.js').write_bytes((root/'Resources'/f'{app}Web/environment.js').read_bytes())
 file=work/'file.txt';file.write_text('HTTP binary ☃')
 env=dict(os.environ,XDG_STATE_HOME=str(work/'state'))
 process=subprocess.Popen([backend,'--socket-path',str(sock),'--web-root',str(web)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,env=env)
 def request(body,guard=True):
  c=Connection(str(sock))
  try:
   c.putrequest('POST','/api/environments');c.putheader('Content-Length',str(len(body)))
   c.putheader('Content-Type','application/octet-stream')
   if guard:c.putheader('X-Environment-Request','1')
   c.endheaders(body);r=c.getresponse();return r.status,r.read()
  finally:c.close()
 try:
  for _ in range(100):
   if sock.exists():break
   if process.poll() is not None:raise RuntimeError(process.stderr.read().decode())
   time.sleep(.02)
  op='read' if app=='Plaintext' else 'preview'
  body=encode_client(op,path=str(file))
  status,data=request(body)
  assert status==200,(status,data)
  result=decode_client(data);assert result.get('contents')=='HTTP binary ☃',result
  assert request(body,False)[0]==403
  assert 'error' in decode_client(request(b'not a binary packet')[1])
  c=Connection(str(sock));c.request('GET','/web/environment.js');r=c.getresponse()
  assert r.status==200 and b'EnvironmentBinary' in r.read();c.close()
  print(f'PASS {app}: real HTTP binary body, final-header guard, missing guard rejection, malformed packet, codec asset')
 finally:
  process.terminate();process.wait(timeout=5);process.stderr.close()
