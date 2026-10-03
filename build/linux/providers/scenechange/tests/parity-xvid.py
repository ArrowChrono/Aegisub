#!/usr/bin/env python3
"""Validate optimized detector against full-Xvid on deterministic mixed motion."""
import argparse, ctypes as C, importlib.util, pathlib, random
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('smoke',HERE/'smoke-providers.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
parser=argparse.ArgumentParser();parser.add_argument('--full',required=True,type=pathlib.Path);parser.add_argument('--optimized',required=True,type=pathlib.Path);args=parser.parse_args()
full=mod.Provider(args.full); fast=mod.Provider(args.optimized)
w,h=320,256
rng=random.Random(0xD37EC70)
randomplanes=lambda:[rng.randbytes(w*h),rng.randbytes(w*h//4),rng.randbytes(w*h//4)]
frames=[randomplanes()]
for index in range(1,18):
    if index in (6,12,17): frame=randomplanes()
    elif index%2==0:
        frame=[]
        for j,prior in enumerate(frames[-1]):
            width=w if j==0 else w//2
            frame.append(b''.join(prior[row:row+1]+prior[row:row+width-1] for row in range(0,len(prior),width)))
    else:
        frame=[bytearray(v) for v in frames[-1]]
        for pixel in range(index,len(frame[0]),37):frame[0][pixel]=(frame[0][pixel]+1)%256
        frame=list(map(bytes,frame))
    frames.append(frame)

def sequence(provider,threads,slices):
    ctx=provider.create(w,h,threads,slices);result=[]
    try:
        for index,planes in enumerate(frames):
            f=mod.Frame();f.struct_size=C.sizeof(f)
            assert provider.api.acquire(ctx,C.byref(f),provider.error,len(provider.error))==0
            for p,source in zip(f.planes,planes):
                assert p.stride==p.width
                C.memmove(p.data,source,len(source))
            r=mod.Result();r.struct_size=C.sizeof(r)
            assert provider.api.commit(ctx,C.byref(r),provider.error,len(provider.error))==0,provider.error.value
            assert r.frame_index==index
            result.append(r.is_scene_change)
    finally:provider.api.destroy(ctx)
    return result
for threads in (1,4,8):
    for slices in (0,1):
        expected=sequence(full,threads,slices);actual=sequence(fast,threads,slices)
        assert actual==expected,(threads,slices,expected,actual)
        print(f'320x256 random YUV/motion/repeats/cuts: threads={threads}, slices={slices}, full == optimized PASS; cuts={[i for i,x in enumerate(actual) if x]}',flush=True)
