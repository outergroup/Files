"""Binary registry fixture used by helper and browser integration tests."""
import os, pathlib, socket, struct, sys, threading

def encode(rows=(), error=''):
    variable=48+72*len(rows)
    out=bytearray(variable)
    struct.pack_into('<4s7I',out,0,b'OSED',1,0,len(rows),72,48,variable,bool(error))
    def ref(offset,value):
        data=value.encode();struct.pack_into('<II',out,offset,len(out),len(data));out.extend(data)
    ref(32,error)
    for i,row in enumerate(rows):
        assert len(row)==9
        for j,value in enumerate(row):ref(48+72*i+8*j,value)
    struct.pack_into('<I',out,8,len(out))
    return bytes(out)

class RegistryServer:
    def __init__(self,path,response):
        self.path=str(path);self.response=response;self.requests=[];self.stopped=False
        self.socket=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);self.socket.bind(self.path);self.socket.listen();self.socket.settimeout(.1)
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def run(self):
        while not self.stopped:
            try:connection,_=self.socket.accept()
            except socket.timeout:continue
            except OSError:break
            with connection:
                wire=b''
                while len(wire)<6:
                    part=connection.recv(6-len(wire))
                    if not part:break
                    wire+=part
                self.requests.append(wire)
                if wire!=b'\x02\0\0\0\x22\0':continue
                value=self.response()
                connection.sendall(struct.pack('<I',len(value))+value)
    def close(self):
        self.stopped=True;self.thread.join(2);self.socket.close();pathlib.Path(self.path).unlink(missing_ok=True)

if __name__=='__main__':
    server=RegistryServer(sys.argv[1],lambda:pathlib.Path(sys.argv[2]).read_bytes())
    pathlib.Path(sys.argv[1]+'.ready').touch()
    try:threading.Event().wait()
    finally:server.close()
