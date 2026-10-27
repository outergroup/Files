"""Validate the Files binary listing through a compiled Unix backend. Pass its path."""
import socket,struct,subprocess,tempfile,time,sys
from pathlib import Path
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'sub').mkdir();(p/'file.txt').write_bytes(b'test\0bytes'); sockpath=str(p/'api')
 proc=subprocess.Popen([sys.argv[1],'--socket-path',sockpath],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
 try:
  for _ in range(100):
   if Path(sockpath).exists():break
   time.sleep(.02)
  from urllib.parse import quote
  with socket.socket(socket.AF_UNIX) as s:
   s.settimeout(5);s.connect(sockpath);s.sendall(f'GET /api/files?path={quote(d)} HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode());result=b''
   while True:
    x=s.recv(65536)
    if not x:break
    result+=x
  headers,data=result.split(b'\r\n\r\n',1);assert b'200 OK' in headers
  assert struct.unpack_from('<II',data)==(0x534c4646,2)
  def ref(at):
   a,n=struct.unpack_from('<II',data,at);return data[a:a+n].decode()
  names={ref(48+i*48):i for i in range(struct.unpack_from('<I',data,8)[0])}
  row=48+names['file.txt']*48
  assert ref(row+8)==d+'/file.txt' and ref(row+16).startswith('-')
  assert struct.unpack_from('<Q',data,row+32)[0]==10
  assert 'sub' in names
  print('PASS Unix Files listing: shared encoder, directory, path, permissions, byte size')
 finally:
  proc.terminate();proc.wait(timeout=5)
