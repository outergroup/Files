import struct
OPS={'list':1,'read':2,'save':3,'move':4,'signal':6,'targets':7,'preview':8,'draft':9,'places':10,'addPlace':11,'removePlace':12}
def encode_client(operation, **values):
    b=bytearray(72)
    struct.pack_into('<IIII',b,0,0x51434c46,1,0,OPS[operation])
    for field,key,default in [(24,'target','host'),(32,'user',''),(40,'path','~'),(48,'destination',''),(56,'revision',''),(64,'contents','')]:
        value=values.get(key,default)
        data=value.encode() if isinstance(value,str) else value
        struct.pack_into('<II',b,field,len(b),len(data));b.extend(data)
    struct.pack_into('<I',b,8,len(b))
    return bytes(b)
def decode_client(b):
    def n(data,o):return struct.unpack_from('<I',data,o)[0]
    def raw(data,o):
        start,size=struct.unpack_from('<II',data,o);assert start+size<=len(data)
        return data[start:start+size]
    def text(data,o):return raw(data,o).decode()
    assert b[:4]==b'FLCR' and n(b,4)==1 and n(b,8)==len(b)
    if n(b,12):return {'error':text(b,32)}
    meta,body=raw(b,16),raw(b,24)
    if meta[:4]==b'OETP':
        if n(meta,12):return {'error':text(meta,72)}
        names=['id','runtime','container','name','state','image','user']
        return {'targets':[dict((key,text(meta,80+i*56+j*8)) for j,key in enumerate(names)) for i in range(n(meta,16))],
                'errors':[dict(runtime=text(meta,n(meta,44)+i*16),error=text(meta,n(meta,44)+i*16+8)) for i in range(n(meta,40))]}
    assert meta[:4]==b'FLHS' and n(meta,16)==len(meta)
    result=dict(ok=True,path=text(meta,48),revision=text(meta,56),dirty=bool(n(b,40)&1),draftConflict=bool(n(b,40)&2))
    if n(meta,8)==2:result.update(data=body,type=text(b,32),contents=body.decode(errors='replace'))
    if n(meta,8) in (1,10):
        result['parent']=text(meta,56)
        result['entries']=[dict(name=text(meta,64+i*48),path=text(meta,72+i*48),mode=text(meta,80+i*48)) for i in range(n(meta,20))]
    return result

def run_client(driver, operation, env=None, **values):
    import subprocess
    output=subprocess.check_output([driver],input=encode_client(operation,**values),env=env,timeout=30)
    return decode_client(output)
