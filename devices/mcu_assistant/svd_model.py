"""CMSIS-SVD register model with scoped inheritance, clusters and arrays."""
import copy
import re
from pack_model import xml, number


def text(n,tag,default=""):return n.findtext(tag,default).strip()


def merge(base,override):
    out=copy.deepcopy(base);out.attrib.update(override.attrib);out.attrib.pop("derivedFrom",None)
    for child in override:
        if child.tag in ("registers","fields"):
            old=out.find(child.tag)
            if old is not None:out.remove(old);out.append(merge(old,child));continue
        name=child.findtext("name")
        old=next((v for v in out if v.tag==child.tag and (v.findtext("name")==name if name is not None else True)),None)
        if old is not None:out.remove(old)
        out.append(copy.deepcopy(child))
    return out


def expanded(n):
    name=text(n,"name")
    count=number(n.findtext("dim"),1)
    if not 1<=count<=4096:raise ValueError("SVD 数组维度超出范围")
    if count==1 and n.find("dim") is None:return [(name,0)]
    step=number(n.findtext("dimIncrement"))
    idx=text(n,"dimIndex")
    if not idx:indices=[str(i) for i in range(count)]
    elif "," in idx:indices=[s.strip() for s in idx.split(",")]
    elif "-" in idx:
        first,last=idx.split("-",1)
        if first.isdigit() and last.isdigit():indices=[str(i) for i in range(int(first),int(last)+1)]
        elif len(first)==len(last)==1:indices=[chr(i) for i in range(ord(first),ord(last)+1)]
        else:raise ValueError("无法解析 dimIndex："+idx)
    else:indices=[idx]
    if len(indices)!=count:raise ValueError("dimIndex 与 dim 数量不一致")
    return [(name.replace("%s",index) if "%s" in name else name+"["+index+"]",i*step) for i,index in enumerate(indices)]


class Svd:
    def __init__(self,data):
        root=xml(data)
        if root.tag!="device":raise ValueError("不是 CMSIS-SVD device 描述")
        if number(root.findtext("addressUnitBits"),8)!=8:raise ValueError("暂不支持非 8 位地址单位的 SVD")
        self.root=root;self.index={};self.registers={};self.peripherals=[]
        self._index(root,"")
        defaults=self.defaults(root,{"size":number(root.findtext("width"),32),"access":"read-write","resetValue":0})
        for raw in root.findall("peripherals/peripheral"):
            path=text(raw,"name");p=self.resolve(raw,path)
            pd=self.defaults(p,defaults)
            self._index(p,"")
            for pname,delta in expanded(p):
                base=number(p.findtext("baseAddress"))+delta
                item={"name":pname,"base":f"0x{base:08X}","description":text(p,"description"),"count":0}
                before=len(self.registers)
                container=p.find("registers")
                if container is not None:self._registers(container,pname,base,0,pd,path)
                item["count"]=len(self.registers)-before;self.peripherals.append(item)
        if not self.peripherals:raise ValueError("SVD 没有外设描述")

    def _index(self,node,parent):
        named=node.tag in ("peripheral","cluster","register","field","enumeratedValues")
        name=text(node,"name") if named else ""
        path=".".join(v for v in (parent,name) if v)
        if named and name:self.index[path]=node
        for child in node:self._index(child,path)

    def resolve(self,node,path,stack=()):
        ref=node.get("derivedFrom")
        if not ref:return copy.deepcopy(node)
        if path in stack:raise ValueError("SVD derivedFrom 循环："+path)
        parent=path.rsplit(".",1)[0] if "." in path else ""
        candidates=[ref] if "." in ref else [".".join(p for p in (".".join(parent.split(".")[:i]),ref) if p) for i in range(len(parent.split(".")),-1,-1)]
        target=next((r for r in candidates if r in self.index),None)
        if target is None:raise ValueError(f"SVD 找不到继承对象 {path} → {ref}")
        base=self.resolve(self.index[target],target,stack+(path,))
        return merge(base,node)

    @staticmethod
    def defaults(node,parent):
        out=dict(parent)
        for key in ("size","access","resetValue","resetMask"):
            val=node.findtext(key)
            if val is not None:out[key]=val.strip() if key=="access" else number(val)
        return out

    def _registers(self,container,prefix,base,offset,defaults,template_prefix):
        for raw in container:
            if raw.tag not in ("register","cluster"):continue
            template_path=template_prefix+"."+text(raw,"name")
            node=self.resolve(raw,template_path)
            self._index(node,template_prefix)
            props=self.defaults(node,defaults)
            for name,delta in expanded(node):
                path=prefix+"."+name
                at=offset+number(node.findtext("addressOffset"))+delta
                if node.tag=="cluster":
                    self._registers(node,path,base,at,props,template_path)
                    continue
                size=props["size"]
                if not 1<=size<=64:raise ValueError("寄存器位宽不支持："+path)
                if path in self.registers:raise ValueError("重复寄存器路径："+path)
                self._index(node,template_prefix)
                fields=[]
                for fr in node.findall("fields/field"):
                    f=self.resolve(fr,template_path+"."+text(fr,"name"))
                    self._index(f,template_path)
                    for fn,bit_delta in expanded(f):
                        if f.find("bitRange") is not None:
                            match=re.fullmatch(r"\[(\d+)\s*:\s*(\d+)\]",text(f,"bitRange"))
                            if not match:raise ValueError("无效 bitRange："+path)
                            high,low=map(int,match.groups());width=high-low+1
                        elif f.find("lsb") is not None:
                            low=number(f.findtext("lsb"));width=number(f.findtext("msb"))-low+1
                        else:low=number(f.findtext("bitOffset"));width=number(f.findtext("bitWidth"),1)
                        low+=bit_delta
                        if low<0 or width<1 or low+width>size:raise ValueError("位域超出寄存器范围："+path+"."+fn)
                        special=f.findtext("modifiedWriteValues") or f.findtext("readAction") or text(node,"modifiedWriteValues") or text(node,"readAction")
                        access=text(f,"access",props["access"])
                        enum=[]
                        for raw_enum in f.findall("enumeratedValues"):
                            ev=self.resolve(raw_enum,template_path+"."+text(fr,"name")+"."+text(raw_enum,"name","@enum"))
                            for e in ev.findall("enumeratedValue"):
                                enum.append({"name":text(e,"name"),"value":text(e,"value"),"description":text(e,"description")})
                        wc=f.find("writeConstraint/range")
                        fields.append({"name":fn,"offset":low,"width":width,"access":access,"description":text(f,"description"),
                            "enum":enum,"special":special,"min":number(wc.findtext("minimum"),0) if wc is not None else 0,
                            "max":number(wc.findtext("maximum"),(1<<width)-1) if wc is not None else (1<<width)-1})
                value=props.get("resetValue",0)&((1<<size)-1)
                self.registers[path]={"path":path,"name":name,"peripheral":prefix.split(".")[0],"address_value":base+at,
                    "address":f"0x{base+at:08X}","size":size,"access":props["access"],"reset":value,
                    "reset_mask":props.get("resetMask",(1<<size)-1),"description":text(node,"description"),"fields":fields,
                    "special":text(node,"modifiedWriteValues") or text(node,"readAction")}
                if len(self.registers)>100000:raise ValueError("SVD 寄存器数量超过 100000")
