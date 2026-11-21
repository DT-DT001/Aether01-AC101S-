"""Original 32x32 constrained-baseline IDR/I_PCM MP4; no encoder dependency."""
from pathlib import Path
import struct

def u32(*x): return struct.pack(">"+"I"*len(x),*x)
def u16(*x): return struct.pack(">"+"H"*len(x),*x)
def box(tag,data): return u32(len(data)+8)+tag.encode()+data
def full(tag,data,flags=0): return box(tag,u32(flags)+data)
class Bits:
    def __init__(self): self.value=[]
    def bits(self,n,v): self.value += [(v>>i)&1 for i in reversed(range(n))]
    def ue(self,v):
        v+=1; n=v.bit_length(); self.bits(n-1,0); self.bits(n,v)
    def se(self,v): self.ue(-2*v if v<=0 else 2*v-1)
    def align(self):
        while len(self.value)%8: self.bits(1,0)
    def data(self):
        self.bits(1,1); self.align()
        return bytes(sum(self.value[i+j]<<(7-j) for j in range(8)) for i in range(0,len(self.value),8))
def nal(header,rbsp):
    out=bytearray([header]); zeros=0
    for b in rbsp:
        if zeros>=2 and b<=3: out.append(3); zeros=0
        out.append(b); zeros=zeros+1 if b==0 else 0
    return bytes(out)
def sps():
    b=Bits(); b.bits(8,66); b.bits(8,0xc0); b.bits(8,12)
    b.ue(0); b.ue(0); b.ue(2); b.ue(1); b.bits(1,0); b.ue(1); b.ue(1)
    b.bits(1,1); b.bits(1,1); b.bits(1,0); b.bits(1,0)
    return nal(0x67,b.data())
def pps():
    b=Bits(); b.ue(0); b.ue(0); b.bits(1,0); b.bits(1,0); b.ue(0)
    b.ue(0); b.ue(0); b.bits(1,0); b.bits(2,0); b.se(0); b.se(0); b.se(0)
    b.bits(1,1); b.bits(1,0); b.bits(1,0)
    return nal(0x68,b.data())
def picture(index):
    b=Bits(); b.ue(0); b.ue(2); b.ue(0); b.bits(4,0); b.ue(index%2)
    b.bits(1,0); b.bits(1,0); b.se(0); b.ue(1)
    for mb in range(4):
        b.ue(25); b.align()
        for _ in range(256): b.bits(8,16+index*9+mb*3)
        for _ in range(128): b.bits(8,128)
    n=nal(0x65,b.data()); return u32(len(n))+n
def predictive(frame_number):
    # Original P-skip slice: all four macroblocks reference the preceding frame.
    # SPS POC type2, one reference, no B/FMO/ASO/redundant slice machinery.
    b=Bits(); b.ue(0); b.ue(0); b.ue(0); b.bits(4,frame_number)
    b.bits(1,0); b.bits(1,0); b.bits(1,0); b.se(0); b.ue(1); b.ue(4)
    n=nal(0x41,b.data()); return u32(len(n))+n
MATRIX=u32(0x10000,0,0,0,0x10000,0,0,0,0x40000000)
def descriptor(tag,data):
    assert len(data)<128
    return bytes([tag,len(data)])+data
def track(kind,tid,samples,offsets,timescale,delta,profile=66,edit=0):
    duration=len(samples)*delta
    tkhd=full("tkhd",u32(0,0,tid,0,duration*1000//timescale)+bytes(8)+u16(0,0,0x100 if kind=="soun" else 0,0)+MATRIX+u32(32<<16 if kind=="vide" else 0,32<<16 if kind=="vide" else 0),7)
    mdhd=full("mdhd",u32(0,0,timescale,duration)+u16(0x55c4,0))
    hdlr=full("hdlr",u32(0)+kind.encode()+bytes(12)+b"Aether original\0")
    if kind=="vide":
        ss,pp=sps(),pps()
        avcc=box("avcC",bytes([1,profile,0xc0,12,0xff,0xe1])+u16(len(ss))+ss+bytes([1])+u16(len(pp))+pp)
        entry=box("avc1",bytes(6)+u16(1)+bytes(16)+u16(32,32)+u32(0x480000,0x480000,0)+u16(1)+bytes(32)+u16(24,0xffff)+avcc)
        media=full("vmhd",bytes(8),1)
    else:
        asc=b"\x11\x90" # AAC LC 48k stereo
        es=descriptor(3,u16(tid)+b"\0"+descriptor(4,b"\x40\x15"+bytes(3)+u32(64000,64000)+descriptor(5,asc))+descriptor(6,b"\2"))
        entry=box("mp4a",bytes(6)+u16(1)+bytes(8)+u16(2,16,0,0)+u32(48000<<16)+full("esds",es))
        media=full("smhd",bytes(4))
    stsd=full("stsd",u32(1)+entry)
    stts=full("stts",u32(1,len(samples),delta))
    stsc=full("stsc",u32(1,1,1,1))
    stsz=full("stsz",u32(0,len(samples),*[len(s) for s in samples]))
    stco=full("stco",u32(len(offsets),*offsets))
    stss=full("stss",u32(len(samples),*range(1,len(samples)+1))) if kind=="vide" else b""
    dinf=box("dinf",full("dref",u32(1)+full("url ",b"",1)))
    minf=box("minf",media+dinf+box("stbl",stsd+stts+stsc+stsz+stco+stss))
    edts=box("edts",full("elst",u32(1,duration*1000//timescale,edit)+u16(1,0)))
    return box("trak",tkhd+edts+box("mdia",mdhd+hdlr+minf))
def create(audio=False,profile=66,audio_only=False,gop=False,padding=0,audio_units=48,bad_seek=False,audio_edit=0,video_count=12):
    ftyp=box("ftyp",b"isom"+u32(0x200)+b"isomiso2avc1mp41")
    video=[] if audio_only else [(predictive(i%6) if gop and i%6 else picture(i%12)) for i in range(video_count)]
    if padding:
        filler=b'\x0c'+b'\x55'*padding
        video=[v+u32(len(filler))+filler for v in video]
    if bad_seek: video[9]=u32(0)+video[9][4:]
    # Original stereo LC silent access unit; no third-party recording.
    audios=[bytes.fromhex("21000000001c")]*audio_units if audio else []
    offset=len(ftyp)+8; vo=[]; ao=[]
    for p in video: vo.append(offset); offset+=len(p)
    for p in audios: ao.append(offset); offset+=len(p)
    mdat=box("mdat",b"".join(video+audios))
    mvhd=full("mvhd",u32(0,0,1000,1000,0x10000)+u16(0x100,0)+bytes(8)+MATRIX+bytes(24)+u32(3))
    moov=box("moov",mvhd+(track("vide",1,video,vo,90000,7500,profile) if video else b"")+(track("soun",2,audios,ao,48000,1024,edit=audio_edit) if audio else b""))
    return ftyp+mdat+moov
def create_fragmented(gap=False,bad_size=False,bad_offset=False,bad_base=False,header=False,cross=False,wrap=False,units=16):
    # Typical iso5 initialization + moof/mdat pairs. Exercise trex defaults,
    # tfhd overrides, 64-bit tfdt and an explicit absolute base-data-offset.
    ftyp=box("ftyp",b"iso5"+u32(1)+b"iso5iso6mp41")
    init=box("moov",track("soun",2,[],[],48000,1024)+box("mvex",full("trex",u32(2,1,1024,6,0))))
    out=ftyp+init; au=bytes.fromhex("21000000001c")
    for index in range(3):
        stamp=index*units*1024+(1024 if gap and index==1 else 0)
        if wrap and index==0: stamp=(1<<64)-1024
        tfdt=full("tfdt",struct.pack(">Q",stamp),0x01000000) if index==1 or wrap else full("tfdt",u32(stamp))
        def fragment(offset=0,base=0):
            if index==2:
                tfhd=full("tfhd",u32(2)+struct.pack(">Q",(1<<63)-1 if bad_base else base),1)
                trun=full("trun",u32(units,0x7fffffff),1) if bad_base else full("trun",u32(units))
            else:
                tfhd=full("tfhd",u32(2,1024,6),0x020018) if index==1 else full("tfhd",u32(2),0x020000)
                entries=b"".join(u32(1024,9000 if bad_size and n==0 else 6) for n in range(units)) if index==1 else b""
                trun=full("trun",u32(units,offset&0xffffffff)+entries,0x301 if index==1 else 1)
            return box("moof",full("mfhd",u32(index+1))+box("traf",tfhd+tfdt+trun))
        proto=fragment(); offset=len(proto)+8
        if bad_offset and index==0: offset=-0x7fffffff
        if header and index==0: offset=0
        if cross and index==0: offset+=units*6-3
        moof=fragment(offset,len(out)+len(proto)+8)
        out+=moof+box("mdat",au*units)
    return out
if __name__=="__main__":
    import sys
    out=Path(sys.argv[1]); out.mkdir(parents=True,exist_ok=True)
    (out/"baseline.mp4").write_bytes(create())
    (out/"baseline-aac.mp4").write_bytes(create(True))
    (out/"baseline-gop.mp4").write_bytes(create(gop=True))
    (out/"baseline-long-gop.mp4").write_bytes(create(gop=True,video_count=60))
    (out/"baseline-gop-aac.mp4").write_bytes(create(True,gop=True))
    (out/"baseline-padded-gop.mp4").write_bytes(create(gop=True,padding=65536))
    (out/"baseline-short-audio.mp4").write_bytes(create(True,gop=True,audio_units=12))
    (out/"baseline-primed-aac.mp4").write_bytes(create(True,gop=True,audio_edit=1024))
    (out/"invalid-seek-nal.mp4").write_bytes(create(gop=True,bad_seek=True))
    (out/"high-profile.mp4").write_bytes(create(profile=100))
    (out/"high-profile-aac.mp4").write_bytes(create(True,profile=100))
    (out/"audio-only.m4a").write_bytes(create(True,audio_only=True))
    (out/"iso5-fragmented.mp3").write_bytes(create_fragmented())
    (out/"iso5-gap.m4a").write_bytes(create_fragmented(gap=True))
    (out/"iso5-size.m4a").write_bytes(create_fragmented(bad_size=True))
    (out/"iso5-offset.m4a").write_bytes(create_fragmented(bad_offset=True))
    (out/"iso5-base.m4a").write_bytes(create_fragmented(bad_base=True))
    (out/"iso5-header.m4a").write_bytes(create_fragmented(header=True))
    (out/"iso5-cross.m4a").write_bytes(create_fragmented(cross=True))
    (out/"iso5-wrap.m4a").write_bytes(create_fragmented(wrap=True))
    (out/"iso5-long.m4a").write_bytes(create_fragmented(units=512))
    (out/"fragmented.mp4").write_bytes(box("ftyp",b"isom"+u32(0)+b"isom")+box("moof",b""))
    stream=b"\0\0\0\1"+sps()+b"\0\0\0\1"+pps()
    stream+=b"\0\0\0\1"+picture(0)[4:]
    (out/"original-idr.h264").write_bytes(stream)
    target=Path(__file__).parent/"target"/"fixture.h"
    text="/* Original generated 32x32 constrained-baseline SPS/PPS/I_PCM IDR. */\n#pragma once\n#include <stdint.h>\nstatic const uint8_t MP4_H264_FIXTURE[]={\n"
    text+="\n".join("    "+",".join(f"0x{x:02x}" for x in stream[i:i+20])+"," for i in range(0,len(stream),20))+"\n};\n"
    if "--update-header" in sys.argv: target.write_text(text)
    elif not target.exists() or target.read_text()!=text: raise SystemExit("target fixture.h is stale; generate with --update-header")
