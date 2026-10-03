#!/usr/bin/env python3
"""Linux C-ABI and real scene detection checks; ctypes uses the public ABI only."""
import argparse, ctypes as C, hashlib, json, pathlib
I=C.c_int32; U=C.c_uint32; P=C.c_void_p; Z=C.c_size_t; ERR=P
class Plane(C.Structure): _fields_=[('data',P),('stride',I),('width',I),('height',I)]
class Frame(C.Structure): _fields_=[('struct_size',U),('pixel_format',I),('plane_count',I),('data',P),('data_size',Z),('planes',Plane*4)]
class Result(C.Structure): _fields_=[('struct_size',U),('frame_index',C.c_int64),('is_scene_change',I),('transition_kind',I),('confidence',C.c_double)]
Create=C.CFUNCTYPE(P,I,I,P,Z,ERR,Z); Destroy=C.CFUNCTYPE(None,P); Reset=C.CFUNCTYPE(I,P,ERR,Z)
Callback=C.CFUNCTYPE(I,P,I,I); SetCallback=C.CFUNCTYPE(I,P,Callback,P,ERR,Z)
Acquire=C.CFUNCTYPE(I,P,C.POINTER(Frame),ERR,Z); Commit=C.CFUNCTYPE(I,P,C.POINTER(Result),ERR,Z)
class Api(C.Structure): _fields_=[('struct_size',U),('api_version',U),('backend',I),('input_pixel_format',I),('capabilities',U),('reserved',U),('create',Create),('destroy',Destroy),('reset',Reset),('set_callback',SetCallback),('acquire',Acquire),('commit',Commit)]
class Options(C.Structure): _fields_=[('struct_size',U),('use_slices',I),('num_threads',I),('log_path',C.c_char_p)]
assert [C.sizeof(x) for x in [Plane,Frame,Result,Api,Options]] == [24,128,32,72,24]
VERSION=0x10000
SOURCE_ROOT=None

def storage(typ):
    buf=C.create_string_buffer(C.sizeof(typ)+16); C.memset(buf,0xA5,C.sizeof(buf))
    obj=typ.from_buffer(buf); obj.struct_size=C.sizeof(buf)
    return buf,obj

def tail(buf,typ): assert bytes(buf)[C.sizeof(typ):] == bytes([0xA5])*16

class Provider:
    def __init__(self,path):
        self.lib=C.CDLL(str(path.resolve())); self.error=C.create_string_buffer(1024)
        get=self.lib.scenechange_provider_get_api; get.argtypes=[U,C.POINTER(Api),ERR,Z]; get.restype=I
        buf,self.api=storage(Api); self.storage=buf
        assert get(VERSION+1,C.byref(self.api),self.error,len(self.error)) == -5
        self.api.struct_size=4
        assert get(VERSION,C.byref(self.api),self.error,len(self.error)) == -1
        self.api.struct_size=C.sizeof(buf)
        assert get(VERSION,C.byref(self.api),self.error,len(self.error)) == 0,self.error.value
        tail(buf,Api); a=self.api
        assert a.struct_size==72 and a.api_version==VERSION and a.backend in (1,2) and a.input_pixel_format==a.backend and a.capabilities&7==7
        for name in ['create','destroy','reset','set_callback','acquire','commit']: assert bool(getattr(a,name))
    def create(self,w,h,threads=1,slices=0):
        opt=Options(C.sizeof(Options),slices,threads,None)
        ptr=C.byref(opt) if self.api.backend==2 else None; size=C.sizeof(opt) if ptr is not None else 0
        context=self.api.create(w,h,ptr,size,self.error,len(self.error))
        assert context,self.error.value
        return context
    def reset(self,ctx): assert self.api.reset(ctx,self.error,len(self.error))==0,self.error.value
    def push(self,ctx,y,w,h,duplicate=False):
        buf,f=storage(Frame); assert self.api.acquire(ctx,C.byref(f),self.error,len(self.error))==0,self.error.value
        tail(buf,Frame); assert f.struct_size==128 and f.data and f.plane_count==(1 if self.api.backend==1 else 3)
        if duplicate:
            g=Frame(); g.struct_size=128
            assert self.api.acquire(ctx,C.byref(g),self.error,len(self.error))==-1
        for pidx in range(f.plane_count):
            plane=f.planes[pidx]
            assert plane.data and plane.stride>=plane.width
            if pidx==0:
                for row in range(plane.height):
                    source=y[min(row,h-1)*w:(min(row,h-1)+1)*w]
                    source+=source[-1:]*(plane.width-w)
                    C.memmove(plane.data+row*plane.stride,source,len(source))
            else:
                for row in range(plane.height): C.memset(plane.data+row*plane.stride,128,plane.width)
        b,r=storage(Result); code=self.api.commit(ctx,C.byref(r),self.error,len(self.error)); tail(b,Result)
        assert code in (0,1),self.error.value
        assert r.struct_size==32 and r.is_scene_change in (0,1)
        return code,r
    def sequence(self,w,h,frames,threads=1,slices=0):
        ctx=self.create(w,h,threads,slices)
        try:
            results=[]
            for index,y in enumerate(frames):
                code,r=self.push(ctx,y,w,h,index==0); assert code==0 and r.frame_index==index
                results.append(r.is_scene_change)
            return results
        finally:self.api.destroy(ctx)

def run(path):
    p=Provider(path); a=p.api
    print(f'{path.name}: backend={a.backend}, api=0x{a.api_version:x}, ABI layout and negotiation PASS',flush=True)
    assert not a.create(0,48,None,0,p.error,len(p.error))
    if a.backend==2: assert not a.create(65,49,None,0,p.error,len(p.error))
    w,h=(65,49) if a.backend==1 else (64,48)
    ctx=p.create(w,h); notifications=[]
    @Callback
    def callback(user,count,scene):
        notifications.append((count,scene)); return int(count==2)
    try:
        assert a.set_callback(ctx,callback,None,p.error,len(p.error))==0
        empty=Result();empty.struct_size=32
        assert a.commit(ctx,C.byref(empty),p.error,len(p.error))==-1
        for index,expected in enumerate([(0,1),(1,0),(0,0)]):
            code,r=p.push(ctx,bytes([32])*(w*h),w,h,index==0)
            assert (code,r.is_scene_change)==expected and r.frame_index==index
        assert notifications==[(1,1),(2,0),(3,0)]
        p.reset(ctx)
        code,r=p.push(ctx,bytes([32])*(w*h),w,h)
        assert (code,r.frame_index,r.is_scene_change)==(0,0,1) and notifications[-1]==(1,1)
    finally:a.destroy(ctx)
    frames=[bytes([v])*(64*48) for v in [16]*8+[235]*8]
    assert p.sequence(64,48,frames)==[1]+[0]*7+[1]+[0]*7
    print(f'{path.name}: invalid inputs, frame ownership, hard cuts 0/8, cancellation consumption, reset callback retention PASS',flush=True)
    if a.backend==2:
        fixture=json.loads((SOURCE_ROOT/'tests/fixtures/scxvid-golden-v1.json').read_text())
        values={'hard-cut':[16]*8+[235]*8,'black-flash':[96]*6+[16]+[96]*6,'fade-in':[16+219*i//11 for i in range(12)],'fade-out':[235-219*i//11 for i in range(12)],'dissolve':[int(48+160*i/11+0.5) for i in range(12)],'dark-scene':[16]*6+[24]*6+[16]*6}
        for threads in (1,4,8):
            for slices in (0,1):
                for scenario in fixture['scenarios']:
                    frames=[bytes([v])*(64*48) for v in values[scenario['name']]]
                    for y,expected in zip(frames,scenario['frames']):
                        assert hashlib.sha256(y+bytes([128])*(32*24*2)).hexdigest()==expected['sha256']
                    actual=p.sequence(64,48,frames,threads,slices)
                    assert actual==[f['scene_change_prev'] for f in scenario['frames']],(scenario['name'],threads,slices,actual)
        print(f'{path.name}: pinned golden 6 scenarios x threads 1/4/8 x slices off/on PASS',flush=True)
        # Motion and real multiple-worker dimensions, with cross-mode consistency.
        w,h=320,256
        frames=[bytes(((x+(n%12)*3)*7+y*11+(80 if n>=12 else 0))%220+16 for y in range(h) for x in range(w)) for n in range(24)]
        baseline=p.sequence(w,h,frames,1,0)
        for threads in (1,4,8):
            for slices in (0,1): assert p.sequence(w,h,frames,threads,slices)==baseline,(threads,slices)
        print(f'{path.name}: 320x256 motion, 24 frames, 1/4/8 threads x slices off/on parity PASS; cuts={[i for i,v in enumerate(baseline) if v]}',flush=True)
    # Exercise allocation teardown repeatedly.
    for _ in range(20):
        assert p.sequence(64,48,[bytes([16])*3072,bytes([235])*3072])==[1,1]
    print(f'{path.name}: 20 create/process/destroy lifecycle cycles PASS',flush=True)
    return p

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--source-root',required=True,type=pathlib.Path);parser.add_argument('libraries',nargs='+',type=pathlib.Path);args=parser.parse_args()
    SOURCE_ROOT=args.source_root.resolve()
    # Keep handles alive: NativeAOT libraries are process-lifetime components.
    libraries=[run(path) for path in args.libraries]
