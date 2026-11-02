"""Read CMSIS-Pack/PDSC without extracting or executing package content."""
import copy
import posixpath
import re
import struct
from pathlib import Path, PurePosixPath
from zipfile import ZipFile
import xml.etree.ElementTree as ET

MAX_FILE = 32*1024*1024


def number(value, default=None):
    if value is None or str(value).strip() == "":
        if default is not None: return default
        raise ValueError("缺少数值")
    text=str(value).strip().replace("_", "")
    if text.startswith("#"): text="0b"+text[1:]
    try: return int(text, 0) if re.match(r"^[+-]?0[xXbBoO]",text) else int(text,10)
    except ValueError: raise ValueError("无法解析数值："+text) from None


def xml(data):
    if b"<!DOCTYPE" in data.upper() or b"<!ENTITY" in data.upper(): raise ValueError("不支持含 DTD/实体声明的 XML")
    try: root=ET.fromstring(data)
    except ET.ParseError as exc: raise ValueError("XML 解析失败："+str(exc)) from None
    for n in root.iter(): n.tag=n.tag.rsplit("}",1)[-1]
    return root


def safe_path(name):
    name=str(name).replace("\\","/")
    if not name or name.startswith("/") or ":" in name or ".." in PurePosixPath(name).parts:
        raise ValueError("无效的包内路径："+name)
    return str(PurePosixPath(name))


class Pack:
    def __init__(self,path):
        self.path=Path(path).expanduser().resolve();self.archive=None
        if not self.path.is_file(): raise ValueError("文件不存在："+str(self.path))
        try:
            if self.path.suffix.lower()==".pack":
                self.archive=ZipFile(self.path)
                names=[i.filename for i in self.archive.infolist() if not i.is_dir()]
                pdsc=[n for n in names if n.lower().endswith(".pdsc")]
                if len(pdsc)!=1: raise ValueError("Pack 必须包含一个 PDSC 文件")
                self.root=None;self.pdsc=pdsc[0]
            elif self.path.suffix.lower()==".pdsc":
                self.root=self.path.parent;self.pdsc=self.path.name
                names=[str(p.relative_to(self.root)).replace("\\","/") for p in self.root.rglob("*") if p.is_file()]
            else: raise ValueError("请导入 .pack 或已解包目录中的 .pdsc")
            if len(names)>100000: raise ValueError("包内文件数量过多")
            self.names={}
            for name in names:
                normalized=safe_path(name)
                if normalized.casefold() in self.names: raise ValueError("包内存在大小写冲突的文件路径")
                self.names[normalized.casefold()]=name
            self.base=posixpath.dirname(self.pdsc)
            root=xml(self.read(self.pdsc,relative=False))
            if root.tag!="package": raise ValueError("不是 CMSIS-Pack package 描述")
            release=root.find("releases/release")
            self.meta={"name":root.findtext("name", ""),"vendor":root.findtext("vendor", ""),
                       "version":release.get("version","") if release is not None else "", "description":root.findtext("description", "")}
            self.devices={}
            self._walk(root.find("devices"),{})
            if not self.devices: raise ValueError("包内没有器件定义（可能不是 Device Family Pack）")
        except Exception:
            self.close();raise

    def close(self):
        if self.archive is not None:self.archive.close();self.archive=None

    def resolve(self,name): return safe_path(posixpath.join(self.base,safe_path(name)))

    def read(self,name,relative=True):
        normalized=self.resolve(name) if relative else safe_path(name)
        actual=self.names.get(normalized.casefold())
        if actual is None:raise ValueError("包内缺少文件："+normalized)
        if self.archive is not None:
            info=self.archive.getinfo(actual)
            if info.file_size>MAX_FILE:raise ValueError("包内单文件超过 32 MiB："+actual)
            return self.archive.read(actual)
        path=(self.root/actual).resolve()
        if not path.is_relative_to(self.root):raise ValueError("包内文件指向目录外部")
        if path.stat().st_size>MAX_FILE:raise ValueError("单文件超过 32 MiB")
        return path.read_bytes()

    def exists(self,name):
        try:return self.resolve(name).casefold() in self.names
        except ValueError:return False

    def _walk(self,node,parent):
        if node is None:return
        state=copy.deepcopy(parent)
        state.setdefault("attrs",{}).update(node.attrib)
        for tag,keyfields in (("processor",("Pname",)),("debug",("Pname","Punit")),("memory",("id","name","Pname")),
                              ("algorithm",("name","Pname")),("feature",("type","name")),("book",("name",))):
            entries=state.setdefault(tag,{})
            for item in node.findall(tag):
                if tag=="memory":key=(item.get("id") or item.get("name"),item.get("Pname",""))
                elif tag=="feature" and item.get("type","").upper() in {"QFP","QFN","BGA","DIP","SO","SON","CSP","LGA","WLCSP"}:key=(item.get("type"),)
                else:key=tuple(item.get(k,"") for k in keyfields)
                entries[key]=dict(entries.get(key,{}),**item.attrib)
        if node.tag in ("device","variant"):
            name=node.get("Dvariant") or node.get("Dname")
            if name:
                if name in self.devices:raise ValueError("重复器件名："+name)
                self.devices[name]=copy.deepcopy(state)
        for child in node:
            if child.tag in ("family","subFamily","device","variant"):self._walk(child,state)


def inspect_flm(data):
    """Inspect ELF32 little-endian ARM symbols and FlashDevice; never load code."""
    if len(data)<52 or data[:7]!=b"\x7fELF\x01\x01\x01":raise ValueError("仅支持 ELF32 little-endian FLM 文件")
    h=struct.unpack_from("<HHIIIIIHHHHHH",data,16)
    if h[1]!=40:raise ValueError("FLM 不是 ARM ELF")
    offset,entry_size,count=h[5],h[10],h[11]
    if entry_size<40 or count>10000 or offset+entry_size*count>len(data):raise ValueError("FLM 节表无效")
    sections=[struct.unpack_from("<IIIIIIIIII",data,offset+i*entry_size) for i in range(count)]
    def section_bytes(s):
        if s[4]+s[5]>len(data):raise ValueError("FLM 节超出文件范围")
        return data[s[4]:s[4]+s[5]]
    symbols={}
    for s in sections:
        if s[1]!=2:continue
        if s[6]>=len(sections) or s[9]<16:raise ValueError("FLM 符号表无效")
        names=section_bytes(sections[s[6]]);table=section_bytes(s)
        for at in range(0,len(table)-15,s[9]):
            no,value,size,info,other,idx=struct.unpack_from("<IIIBBH",table,at)
            if no>=len(names):continue
            name=names[no:].split(b"\0",1)[0].decode("utf-8",errors="replace")
            if idx:symbols[name]=(value,size,idx)
    functions=[]
    for name in ("Init","UnInit","EraseChip","EraseSector","ProgramPage","BlankCheck","Verify"):
        if name in symbols:functions.append({"name":name,"address":f"0x{symbols[name][0]:08X}","size":symbols[name][1]})
    result={"format":"ELF32 ARM little-endian","bytes":len(data),"functions":functions,"device_name":"未提供 FlashDevice 符号","sectors":[]}
    if "FlashDevice" in symbols:
        value,size,idx=symbols["FlashDevice"]
        if idx>=len(sections):raise ValueError("FlashDevice 节索引无效")
        s=sections[idx];payload=section_bytes(s);at=value-s[3]
        if at<0 or at+160>len(payload):raise ValueError("FlashDevice 结构截断")
        d=payload[at:at+size] if size else payload[at:]
        if len(d)<160:raise ValueError("FlashDevice 结构过短")
        base,total,page=struct.unpack_from("<III",d,132)
        result.update(device_name=d[2:130].split(b"\0",1)[0].decode("utf-8",errors="replace"),flash_start=f"0x{base:08X}",flash_bytes=total,page_bytes=page,erased_value=f"0x{d[148]:02X}")
        for pos in range(160,len(d)-7,8):
            sector,address=struct.unpack_from("<II",d,pos)
            if sector==0xFFFFFFFF or address==0xFFFFFFFF:break
            result["sectors"].append({"size":sector,"offset":f"0x{address:08X}"})
    return result
