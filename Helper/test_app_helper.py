#!/usr/bin/env python3
import hashlib, http.client, os, pathlib, signal, socket, struct, subprocess, sys, tempfile, time
from test_registry_server import RegistryServer, encode
APP = 'Files'
PREFIX = 'FL'
CAPS = 515
HELPER = str(pathlib.Path(sys.argv[1]).resolve())
BACKEND = str(pathlib.Path(sys.argv[2]).resolve())
PROVIDER = sys.argv[3] if len(sys.argv)>3 else 'docker'
assert PROVIDER in ('docker','apple.container','wslc')
RUNTIME_KEY = 'apple' if PROVIDER=='apple.container' else PROVIDER
EXECUTABLE = {'docker':'docker','apple.container':'container','wslc':'wslc.exe'}[PROVIDER]

def exact(stream, n):
 data = b''
 while len(data)<n:
  part=stream.read(n-len(data))
  if not part:raise EOFError()
  data+=part
 return data

def packet(op, **values):
 ops={'list':1,'read':2,'save':3,'move':4,'targets':7,'preview':8,'draft':9}
 b=bytearray(72);b[:4]=(PREFIX+'CQ').encode();struct.pack_into('<III',b,4,1,0,ops[op])
 for i,(k,default) in enumerate([('target','host'),('user',''),('path','~'),('destination',''),('revision',''),('contents','')]):
  value=values.get(k,default).encode();struct.pack_into('<II',b,24+i*8,len(b),len(value));b.extend(value)
 struct.pack_into('<I',b,8,len(b));return b

def ref(b,o):
 start,size=struct.unpack_from('<II',b,o);assert start+size<=len(b);return b[start:start+size]

def decode(b):
 assert b[:4]==(PREFIX+'CR').encode()
 if struct.unpack_from('<I',b,12)[0]:return {'error':ref(b,32).decode()}
 meta=ref(b,16)
 if meta[:4]==b'OETP':return {'targets':struct.unpack_from('<I',meta,16)[0]}
 assert meta[:4]==(PREFIX+'HS').encode()
 return {'path':ref(meta,48).decode(),'revision':ref(meta,56).decode(),'body':ref(b,24),'flags':struct.unpack_from('<I',b,40)[0]}

class Connection(http.client.HTTPConnection):
 def __init__(self,path):super().__init__('localhost',timeout=40);self.path=path
 def connect(self):self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(40);self.sock.connect(self.path)

def alive(pid):
 try:os.kill(pid,0);return True
 except ProcessLookupError:return False

def gone(pids):
 for _ in range(150):
  if not any(alive(pid) for pid in pids):return
  time.sleep(.02)
 assert not any(alive(pid) for pid in pids),pids

with tempfile.TemporaryDirectory(prefix='app-helper-',dir='/tmp') as temp:
 root=pathlib.Path(temp);web=root/'web';web.mkdir()
 (root/'helpers'/'linux-x86_64').mkdir(parents=True);(root/'helpers'/'linux-x86_64'/('outer-'+APP.lower()+'-helper')).write_bytes(pathlib.Path(HELPER).read_bytes())
 (web/'environment.js').write_text('test')
 target='a'*64;row=['test','Container',PROVIDER,target,'running','test','','','registered-name']
 registry=RegistryServer(root/'registry.sock',lambda:encode([row]))
 runtime=root/EXECUTABLE;runtime.write_text('#!'+sys.executable+'\n'+'''import os,sys,pathlib
args=sys.argv[1:]
if args[:1]==['--host']:args=args[2:]
assert args[:2] in (['exec','-i'],['exec','--interactive']),args
args=args[2:]
if args[:1]==['--user']:args=args[2:]
assert args[1:3]==['/bin/sh','-c'] and args[4:7]==['outer-container-helper','org.outershell.APP_NAME','outer-APP_LOWER-helper'] and args[11:]==['serve'],args
import hashlib
uploaded=pathlib.Path(os.environ['TEST_EXEC_LOG']).with_name('uploaded')
if uploaded.exists():os.write(1,b'R\\n')
else:
 os.write(1,b'U x86_64\\n');size=int(args[8]);data=b''
 while len(data)<size:
  part=os.read(0,size-len(data))
  if not part:raise EOFError()
  data+=part
 assert hashlib.sha256(data).hexdigest()==args[7]
 uploaded.touch()
with open(os.environ['TEST_EXEC_LOG'],'a') as f:f.write(str(os.getpid())+'\\n')
os.execv(os.environ['TEST_HELPER'],[os.environ['TEST_HELPER'],'serve'])
'''.replace('APP_LOWER',APP.lower()).replace('APP_NAME',APP));runtime.chmod(0o755)
 env=dict(os.environ,PATH=str(root)+':'+os.environ.get('PATH',''),OUTERSHELLD_API_SOCKET=registry.path,
          TEST_HELPER=HELPER,TEST_EXEC_LOG=str(root/'executions'),XDG_STATE_HOME=str(root/'state'))
 file=root/'document.txt';file.write_text('original café')
 sock=root/'backend.sock'
 process=None
 def start():
  global process
  process=subprocess.Popen([BACKEND,'--socket-path',str(sock),'--web-root',str(web)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
  for _ in range(100):
   if sock.exists():return
   if process.poll() is not None:raise RuntimeError(process.stderr.read().decode())
   time.sleep(.02)
 def call(op=None, **fields):
  c=Connection(str(sock))
  try:
   if APP=='Top':c.request('GET','/api/environments?operation=processes')
   else:
    body=packet(op,**fields);c.putrequest('POST','/api/environments');c.putheader('Content-Length',str(len(body)))
    c.putheader('X-Environment-Request','1');c.endheaders(body)
   response=c.getresponse();data=response.read();assert response.status==200,(response.status,data)
   if APP=='Top':assert data[:4]==b'OETP';return data
   return decode(data)
  finally:c.close()
 def pids():return [int(v) for v in (root/'executions').read_text().splitlines()]
 try:
  # The app's container protocol rejects other apps and operations outside its capability set.
  proc=subprocess.Popen([HELPER,'serve'],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
  h=exact(proc.stdout,16);assert h[:4]==(PREFIX+'H2').encode();hello=exact(proc.stdout,16)
  assert struct.unpack('<IIII',hello)==(2,CAPS,16777216,65536)
  meta=bytearray(80);meta[:4]=b'FLHR';struct.pack_into('<III',meta,4,2,3,80)
  for field in [40,48,56,64]:struct.pack_into('<II',meta,field,80,0)
  proc.stdin.write(b'FLH2'+struct.pack('<III',2,1,len(meta))+meta+b'FLH2'+struct.pack('<III',4,1,0));proc.stdin.flush()
  response=exact(proc.stdout,16);error=exact(proc.stdout,struct.unpack_from('<I',response,12)[0])
  assert struct.unpack_from('<I',error,12)[0]==1
  assert exact(proc.stdout,16)[4:8]==struct.pack('<I',4)
  proc.stdin.write(b'BAD!'+b'\0'*12);proc.stdin.close();assert proc.wait(timeout=3)!=0;proc.stdout.close()
  assert subprocess.run([HELPER,'host'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==2
  start()
  op='list' if APP=='Files' else 'read'
  fields=dict(target=RUNTIME_KEY+':'+target,path=str(root if APP=='Files' else file))
  for _ in range(3):result=call(op,**fields)
  assert registry.requests.count(b'\x02\0\0\0\x22\0')==1,registry.requests
  assert len(pids())==1,pids();assert alive(pids()[0])
  assert int(subprocess.check_output(['ps','-o','ppid=','-p',str(pids()[0])]))==process.pid
  first=pids()[0];os.kill(first,signal.SIGKILL)
  time.sleep(.05);call(op,**fields);assert len(pids())==2,pids()
  if APP=='Plaintext':
   read=call('read',**fields);assert read['body']=='original café'.encode()
   assert 'error' not in call('draft',**fields,revision=read['revision'],contents='unsaved')
   restored=call('read',**fields);assert restored['body']==b'unsaved' and restored['flags']==1
   file.write_text('external');assert call('read',**fields)['flags']==3
   assert 'error' in call('save',**fields,revision=read['revision'],contents='should not overwrite')
   assert file.read_text()=='external'
   # An ordinary operation error must not discard the session.
   assert len(pids())==2
  if APP!='Top':
   row[3]='b'*64
   call('targets')
   assert 'error' in call(op,**fields)
   gone([pids()[-1]])
   row[3]=target;call(op,**fields)
   row[3]='c'*64
   os.kill(pids()[-1],signal.SIGKILL)
   time.sleep(.05)
   before=len(pids())
   assert 'error' in call(op,**fields)
   assert len(pids())==before
   row[3]=target;call(op,**fields)
  active=pids()[-1]
  process.terminate();process.wait(timeout=5);process.stderr.close();gone(pids())
  sock.unlink(missing_ok=True);start();call(op,**fields);active=pids()[-1]
  process.kill();process.wait(timeout=5);process.stderr.close();gone(pids())
  print('PASS',APP,'direct backend, no host executable, distinct protocol, session reuse, container crash recovery, scopes/conflicts, stale identity, backend graceful/crash cleanup')
 finally:
  if process and process.poll() is None:process.kill();process.wait()
  registry.close()
