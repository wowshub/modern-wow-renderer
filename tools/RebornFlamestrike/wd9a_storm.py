import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import pympq
lib=c.WinDLL(str(Path(pympq.__file__).parent/'StormLib.dll'),use_last_error=True)
lib.SFileOpenArchive.argtypes=[c.c_wchar_p,w.DWORD,w.DWORD,c.POINTER(w.HANDLE)];lib.SFileOpenArchive.restype=c.c_bool
lib.SFileOpenFileEx.argtypes=[w.HANDLE,c.c_char_p,w.DWORD,c.POINTER(w.HANDLE)];lib.SFileOpenFileEx.restype=c.c_bool
lib.SFileGetFileSize.argtypes=[w.HANDLE,c.POINTER(w.DWORD)];lib.SFileGetFileSize.restype=w.DWORD
lib.SFileReadFile.argtypes=[w.HANDLE,c.c_void_p,w.DWORD,c.POINTER(w.DWORD),c.c_void_p];lib.SFileReadFile.restype=c.c_bool
lib.SFileCloseFile.argtypes=[w.HANDLE];lib.SFileCloseArchive.argtypes=[w.HANDLE]
class Archive:
 def __init__(self,path):
  self.h=w.HANDLE();assert lib.SFileOpenArchive(str(path),0,0x100,self.h), (path,c.get_last_error())
 def read(self,name):
  h=w.HANDLE()
  if not lib.SFileOpenFileEx(self.h,name.encode('utf8'),0,h):return None
  try:
   high=w.DWORD();size=lib.SFileGetFileSize(h,high);assert not high.value and size<1000000000
   buf=c.create_string_buffer(size);n=w.DWORD();assert lib.SFileReadFile(h,buf,size,n,None) and n.value==size
   return buf.raw
  finally:lib.SFileCloseFile(h)
 def close(self):lib.SFileCloseArchive(self.h)
