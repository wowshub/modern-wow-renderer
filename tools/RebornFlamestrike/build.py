from pathlib import Path
import struct, json, hashlib, shutil, datetime, math, zipfile
import pympq
from wd9a_storm import Archive

import argparse
parser=argparse.ArgumentParser(description='Build the accepted LIGHT8B client resource patch without modifying the client.')
parser.add_argument('--client-data', type=Path, required=True, help='blacknight Data directory containing WD135A Patch-XA and patch-Z')
parser.add_argument('--output', type=Path, required=True, help='New output directory outside the client directory')
args=parser.parse_args()
DATA=args.client_data.resolve()
OUT=args.output.resolve()
if OUT == DATA.parent or DATA.parent in OUT.parents:
 parser.error('Output must be outside the client directory.')
OUT.mkdir(parents=True,exist_ok=False)
BASELINE=json.loads((Path(__file__).parent/'baseline-sha256.json').read_text(encoding='utf8'))
def verify_baseline(name,raw):
 assert raw is not None and hashlib.sha256(raw).hexdigest()==BASELINE[name], 'Unsupported baseline: '+name
INPUT=OUT/'client_mpq输入'; BACK=OUT/'rollback_原始资源'
REPORT=[]
def save(root,name,b):
 p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b);return p
def sha(b):return hashlib.sha256(b).hexdigest()
def pair(b,p):return struct.unpack_from('<II',b,p)
def append(b,data):
 b.extend(b'\0'*((-len(b))%16));off=len(b);b.extend(data);return off
def track(b,p,fmt,fn,label):
 # Clone nested value arrays: old models sometimes share track data.
 n,o=pair(b,p+12);desc=bytearray();before=[];after=[]
 width=struct.calcsize('<'+fmt)
 for j in range(n):
  c,v=pair(b,o+j*8);assert v+c*width<=len(b)
  vals=[struct.unpack_from('<'+fmt,b,v+k*width) for k in range(c)]
  new=[tuple(fn(x)) for x in vals]
  assert all(math.isfinite(x) for row in new for x in row)
  nv=append(b,b''.join(struct.pack('<'+fmt,*x) for x in new))
  desc.extend(struct.pack('<II',c,nv));before+=vals;after+=new
 no=append(b,desc);struct.pack_into('<II',b,p+12,n,no)
 REPORT.append(dict(field=label,before=before,after=after))
def scalar(b,p,f,label):track(b,p,'f',lambda v:(v[0]*f,),label)
def scale_y(b,p,f,label):
 c,o=pair(b,p+8);vals=[struct.unpack_from('<2f',b,o+i*8) for i in range(c)]
 new=[(x,y*f) for x,y in vals];no=append(b,b''.join(struct.pack('<2f',*v) for v in new))
 struct.pack_into('<II',b,p+8,c,no)
 REPORT.append(dict(field=label,before=vals,after=new))

names=['cfx_mage_flamestrike','cfx_mage_flamestrike_castworld','cfx_mage_flamestrikeprojected_castworld']
archive=Archive(DATA/'patch-Z.mpq')
for index,name in enumerate(names):
 old=archive.read('Spells\\'+name+'.m2');skin=archive.read('Spells\\'+name+'00.skin')
 verify_baseline('Spells/'+name+'.m2',old)
 verify_baseline('Spells/'+name+'00.skin',skin)
 assert old[:4]==b'MD20' and struct.unpack_from('<I',old,4)[0]==264
 save(BACK,'Spells/'+name+'.m2',old);save(BACK,'Spells/'+name+'00.skin',skin)
 b=bytearray(old);n,base=pair(b,296);assert n==5 and base+n*476<=len(b)
 def p(i,offset):return base+476*i+offset
 if index==0:
  for i in [0,1]:
   scalar(b,p(i,52),1.25,name+f'.fire{i}.speed')
   scalar(b,p(i,152),1.25,name+f'.fire{i}.life')
   scalar(b,p(i,176),0.8,name+f'.fire{i}.rate')
   scale_y(b,p(i,292),1.65,name+f'.fire{i}.height')
  for i in [2,3]:
   scalar(b,p(i,152),1.15,name+f'.smoke{i}.life')
   scalar(b,p(i,176),0.8,name+f'.smoke{i}.rate')
 elif index==1:
  scalar(b,p(2,152),1.35,name+'.ember.life')
  scalar(b,p(2,176),0.6,name+'.ember.rate')
  scalar(b,p(4,152),1.4,name+'.smoke.life')
  scalar(b,p(4,176),0.7,name+'.smoke.rate')
 else:
  for i in [1,3,4]:
   scalar(b,p(i,152),1.5,name+f'.ember{i}.life')
   scalar(b,p(i,176),0.25 if i==1 else 1.0,name+f'.ember{i}.rate')
   scalar(b,p(i,132),0.65,name+f'.ember{i}.gravity')
  nc,co=pair(b,72);assert nc==2
  # Skin batch 0 uses color 0, texture lookup 0 => texture 2 (NoGlow).
  nt,to=pair(b,128);assert struct.unpack_from('<H',b,to)[0]==2
  track(b,co,'3f',lambda v:(v[0]*.38,v[1]*.32,v[2]*.28),name+'.scorch.tint')
  track(b,co+20,'h',lambda v:(min(32767,round(v[0]*1.12)),),name+'.scorch.alpha')
  # Color 1 also drives animated fire layers; preserve it unchanged.
 # Particle location, emission dimensions, topology and texture paths stay intact.
 for i in range(n):
  for rel,size in [(8,12),(200,20),(220,20)]:
   assert b[p(i,rel):p(i,rel)+size]==old[p(i,rel):p(i,rel)+size]
 nv,vo=pair(old,60);assert b[vo:vo+nv*48]==old[vo:vo+nv*48]
 newname='reborn_light8_'+name
 save(INPUT,'Spells/'+newname+'.m2',b);save(INPUT,'Spells/'+newname+'00.skin',skin)
archive.close()

class DBC:
 def __init__(self,name,archive):
  self.name=name;self.raw=archive.read('DBFilesClient\\'+name+'.dbc')
  verify_baseline('DBFilesClient/'+name+'.dbc',self.raw)
  magic,self.n,self.f,self.s,ss=struct.unpack_from('<4s4I',self.raw)
  assert magic==b'WDBC' and self.s==self.f*4 and len(self.raw)==20+self.n*self.s+ss
  self.rows=[list(struct.unpack_from('<'+'I'*self.f,self.raw,20+i*self.s)) for i in range(self.n)]
  self.by={r[0]:r for r in self.rows};assert len(self.by)==self.n
  self.strings=bytearray(self.raw[20+self.n*self.s:])
  save(BACK,'DBFilesClient/'+name+'.dbc',self.raw)
 def clone(self,old):
  r=self.by[old].copy();r[0]=max(self.by)+1;self.rows.append(r);self.by[r[0]]=r;return r
 def write(self):
  raw=struct.pack('<4s4I',b'WDBC',len(self.rows),self.f,self.s,len(self.strings))+b''.join(struct.pack('<'+'I'*self.f,*r) for r in self.rows)+self.strings
  save(INPUT,'DBFilesClient/'+self.name+'.dbc',raw)
  return raw
a=Archive(DATA/'Patch-XA.mpq')
s,v,k,e=[DBC(x,a) for x in ['Spell','SpellVisual','SpellVisualKit','SpellVisualEffectName']];a.close()
assert 9003953 in s.by
effects={}
for old,name in zip([13043,12783,12784],names):
 r=e.clone(old);r[2]=len(e.strings);e.strings.extend(('Spells\\reborn_light8_'+name+'.mdx\0').encode());effects[old]=r[0]
ground=k.clone(23527);ground[5]=effects[13043];ground[14]=effects[12784]
cast=k.clone(23526);cast[5]=effects[12783]
visual=v.clone(23372);visual[24]=ground[0];visual[25]=cast[0]
ranks=[2120,2121,8422,8423,10215,10216,27086,42925,42926]
for sid in ranks:assert s.by[sid][131]==23372;s.by[sid][131]=visual[0]
for table in [s,v,k,e]:
 raw=table.write()
 for i in range(table.n):
  old=list(struct.unpack_from('<'+'I'*table.f,table.raw,20+i*table.s));new=table.rows[i]
  diffs=[j for j in range(table.f) if old[j]!=new[j]]
  assert diffs==([131] if table is s and old[0] in ranks else []),(table.name,old[0],diffs)
mpq=OUT/'02_覆盖到客户端根目录/Data/patch-ZZ.mpq';mpq.parent.mkdir(parents=True)
a=pympq.create_archive(str(mpq),[pympq.MPQ_CREATE_ARCHIVE_V1],64)
files=sorted(p for p in INPUT.rglob('*') if p.is_file())
for f in files:a.add_file(str(f),str(f.relative_to(INPUT)).replace('/','\\'),[pympq.MPQ_FILE_COMPRESS],[pympq.MPQ_COMPRESSION_ZLIB])
a.close()
a=Archive(mpq)
for f in files:assert a.read(str(f.relative_to(INPUT)).replace('/','\\'))==f.read_bytes()
a.close()
manifest={str(f.relative_to(OUT)):dict(size=f.stat().st_size,sha256=sha(f.read_bytes())) for f in files+[mpq]}
checks=OUT/'checks';checks.mkdir()
(checks/'changes.json').write_text(json.dumps(REPORT,ensure_ascii=False,indent=2),encoding='utf8')
(checks/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf8')
(checks/'validation.json').write_text(json.dumps(dict(status='offline_pass_game_pending',spell_ids=ranks,visual_id=visual[0],effects=effects,kit_ids=[ground[0],cast[0]],original_rows_preserved=True,mpq_readback=True,vertices_unchanged=True,skins_unchanged=True,client_not_modified=True),indent=2),encoding='utf8')
tools=OUT/'tools';tools.mkdir();shutil.copy2(__file__,tools/'build_light8.py');shutil.copy2(Path(__file__).parent/'wd9a_storm.py',tools/'wd9a_storm.py')
print(OUT)
print(json.dumps(dict(visual=visual[0],kits=[ground[0],cast[0]],effects=effects,mpq_size=mpq.stat().st_size)))
