"""MCU Assistant: offline pack inspection and register initialization planning."""
import csv
import json
import io
from pathlib import Path

from dm.plugin import Plugin, method
from pack_model import Pack, inspect_flm, xml, number
from svd_model import Svd

PACKAGE_TYPES={"QFP","QFN","BGA","DIP","SO","SON","CSP","LGA","WLCSP"}


def writable(access):return access in ("read-write","write-only","writeOnce","read-writeOnce")


class McuAssistant(Plugin):
    def __init__(self,config):
        super().__init__(config)
        self.pack=None;self.device="";self.processor="";self.model=None;self.svd_path="";self.svd_error=""
        self.peripheral="";self.register="";self.configured={};self.pins=[];self.pin_source="未提供逐引脚数据";self.rev=0
        self.device_query=""

    def close(self):
        if self.pack:self.pack.close()

    def state(self):
        if not self.pack or not self.device:raise ValueError("请先导入 MCU Pack")
        return self.pack.devices[self.device]

    def matching(self,tag):
        return [v for v in self.state().get(tag,{}).values() if not v.get("Pname") or v["Pname"]==self.processor]

    def registers_model(self):
        if not self.model:raise ValueError(self.svd_error or "当前器件没有可用 SVD")
        return self.model

    def _select(self,name):
        if name not in self.pack.devices:raise ValueError("器件不存在："+name)
        self.device=name
        processors=list(self.state().get("processor",{}).values())
        self.processor=processors[0].get("Pname","") if processors else ""
        self.pins=[];self.pin_source="Pack 未提供逐引脚复用表，可补充 CSV / 厂商 MCU XML"
        self._svd()

    def _svd(self):
        self.model=None;self.svd_path="";self.svd_error="";self.peripheral="";self.register=""
        debug=self.matching("debug")
        specific=[d for d in debug if d.get("Pname")==self.processor]
        for d in specific+debug:
            if d.get("svd"):self.svd_path=d["svd"];break
        if self.svd_path:
            try:
                self.model=Svd(self.pack.read(self.svd_path))
                self.peripheral=self.model.peripherals[0]["name"]
                self.register=next((k for k,v in self.model.registers.items() if v["peripheral"]==self.peripheral),"")
            except (ValueError,KeyError) as exc:self.svd_error="SVD 无法完整解析："+str(exc)
        else:self.svd_error="Pack 未提供该内核的 SVD；器件及烧录算法仍可浏览"
        self.rev+=1

    @method("导入 CMSIS .pack 或已解包的 .pdsc")
    def import_pack(self,path:str):
        candidate=Pack(path)
        previous=self.pack
        self.pack=candidate;self.configured={};self.device_query=""
        self._select(next(iter(candidate.devices)))
        if previous:previous.close()
        self.log(f"已导入 {candidate.meta['name']}：{len(candidate.devices)} 个器件/变体")
        return self.info()

    @method("载入教学示例（虚拟 MCU，不包含可执行烧录算法）")
    def demo(self):
        root=Path(self.config.get("plugin_dir") or Path(__file__).parent)/"examples"
        self.import_pack(str(root/"DemoMCU.pdsc"));self.import_pins(str(root/"pins.csv"))
        return self.info()

    @method("器件选择列表，可按型号包含关系筛选")
    def device_options(self,query:str=None):
        query=self.device_query if query is None else query
        values=[]
        if self.pack:
            values=[{"label":n,"value":n} for n in self.pack.devices if query.casefold() in n.casefold()]
        return {"devices":values}

    @method("过滤器件列表，不改变当前已打开的器件")
    def filter_devices(self,query:str=""):
        self.device_query=query;return self.device_options()

    @method("切换 MCU 器件/封装变体")
    def select_device(self,name:str):
        self.state();self._select(name);return self.info()

    @method("处理器内核选项")
    def processor_options(self):
        rows=[]
        if self.pack:
            rows=[{"label":p.get("Pname","默认内核")+" · "+p.get("Dcore",""),"value":p.get("Pname","")} for p in self.state().get("processor",{}).values()]
        return {"processors":rows}

    @method("切换多核 MCU 的处理器")
    def select_processor(self,name:str):
        if name not in [p.get("Pname","") for p in self.state().get("processor",{}).values()]:raise ValueError("内核不存在")
        self.processor=name;self._svd();return self.info()

    @method("Pack、封装、引脚数、内核及寄存器解析状态")
    def info(self):
        base={"loaded":bool(self.pack),"rev":self.rev,"device":self.device,"processor":self.processor,"peripheral":self.peripheral,"register":self.register,
              "pack":"未导入 Pack","pin_count":"未知","pin_source":self.pin_source,"pin_rows":len(self.pins),
              "svd_status":self.svd_error or "请导入 Pack","registers":0,"peripherals":0,"memory":[],"features":[],"documents":[],"core":"未提供"}
        if not self.pack:return base
        state=self.state();features=list(state["feature"].values())
        physical=[f for f in features if f.get("type","").upper() in PACKAGE_TYPES]
        base.update(pack=self.pack.meta["vendor"]+"."+self.pack.meta["name"]+" "+self.pack.meta["version"],pack_path=str(self.pack.path),
            device_count=len(self.pack.devices),core=" / ".join(p.get("Dcore","") for p in self.matching("processor")),
            family=state["attrs"].get("Dfamily",""),vendor=state["attrs"].get("Dvendor",self.pack.meta["vendor"]),
            pin_count=" / ".join(str(f.get("count") or f.get("n") or "未知")+" ("+f.get("type","")+")" for f in physical) or "Pack 未提供封装引脚数",
            gpio_count=" / ".join(f.get("n","") for f in features if f.get("type","").casefold()=="ios") or "未提供",
            svd=self.svd_path,svd_status=self.svd_error or "SVD 已解析；以下数值为离线复位值/配置值，不是芯片实时读数",
            registers=len(self.model.registers) if self.model else 0,peripherals=len(self.model.peripherals) if self.model else 0,
            memory=[dict(m,start_hex=f"0x{number(m.get('start'),0):08X}",bytes=number(m.get("size"),0)) for m in self.matching("memory")],
            features=features,documents=[dict(b,exists=self.pack.exists(b.get("name",""))) for b in state["book"].values()])
        return base

    @method("Flash 烧录算法及覆盖范围")
    def algorithms(self):
        if not self.pack:return {"rows":[]}
        rows=[]
        for a in self.matching("algorithm"):
            name=a.get("name","")
            rows.append({"name":name,"start":f"0x{number(a.get('start'),0):08X}","bytes":number(a.get("size"),0),"default":a.get("default","0") in ("1","true"),
                         "style":a.get("style","Keil"),"exists":self.pack.exists(name),"ram_start":a.get("RAMstart","未指定"),"ram_size":a.get("RAMsize","未指定")})
        return {"rows":sorted(rows,key=lambda r:not r["default"])}

    @method("FLM 选择列表")
    def algorithm_options(self):return {"algorithms":[{"label":r["name"],"value":r["name"]} for r in self.algorithms()["rows"]]}

    @method("解析 FLM ELF 符号、FlashDevice、页和扇区；不执行代码")
    def inspect_algorithm(self,name:str):
        if name not in [a["name"] for a in self.algorithms()["rows"]]:raise ValueError("不是当前器件的烧录算法")
        result=inspect_flm(self.pack.read(name));result["name"]=name;return result

    @method("导出当前器件指定的烧录算法文件，不覆盖已有文件")
    def export_algorithm(self,name:str,path:str):
        if name not in [a["name"] for a in self.algorithms()["rows"]]:raise ValueError("不是当前器件的烧录算法")
        if not path.strip():raise ValueError("请填写导出路径")
        data=self.pack.read(name);out=Path(path).expanduser();out.parent.mkdir(parents=True,exist_ok=True)
        with out.open("xb") as file:file.write(data)
        return {"path":str(out.resolve())}

    @method("导入逐引脚表：CSV(number,name,functions,type) 或 STM32 MCU XML")
    def import_pins(self,path:str):
        self.state();file=Path(path).expanduser()
        if not file.is_file() or file.stat().st_size>32*1024*1024:raise ValueError("引脚文件不存在或超过 32 MiB")
        rows=self._parse_pins(file.read_bytes(),file.suffix.lower())
        self.pins=rows;self.pin_source=str(file.resolve());self.rev+=1
        return {"rows":rows,"source":self.pin_source}

    @staticmethod
    def _parse_pins(data,suffix):
        rows=[]
        if suffix==".csv":
            reader=csv.DictReader(io.StringIO(data.decode("utf-8-sig")))
            if not {"number","name","functions"}.issubset(reader.fieldnames or []):raise ValueError("CSV 需要 number,name,functions 列，可选 type 列")
            rows=[{"number":r["number"] or "","name":r["name"] or "","functions":r["functions"] or "","type":r.get("type","") or ""} for r in reader]
        elif suffix==".xml":
            root=xml(data)
            for p in root.iter():
                if p.tag.casefold()!="pin":continue
                funcs=[s.get("Name") or s.get("name") for s in p if s.tag.casefold() in ("signal","function")]
                rows.append({"number":p.get("Position") or p.get("position") or p.get("number", ""),"name":p.get("Name") or p.get("name", ""),
                             "functions":" / ".join(v for v in funcs if v),"type":p.get("Type") or p.get("type","")})
        else:raise ValueError("引脚表支持 .csv 或 .xml")
        if not rows or len(rows)>10000:raise ValueError("引脚表没有有效条目或条目过多")
        if any(not r["number"].strip() or not r["name"].strip() for r in rows):raise ValueError("每个引脚必须有编号和名称")
        return rows

    @method("包内可能提供引脚信息的 XML/CSV 文件；需选择匹配当前器件的厂商文件")
    def pin_file_options(self):
        names=[]
        if self.pack:
            names=[n for n in self.pack.names.values() if Path(n).suffix.lower() in (".csv",".xml") and any(k in n.casefold() for k in ("pin","mcu","gpio"))]
        return {"files":[{"label":n,"value":n} for n in names]}

    @method("读取包内厂商引脚 XML/CSV，不解压或执行")
    def import_pack_pins(self,name:str):
        self.state()
        if name not in [r["value"] for r in self.pin_file_options()["files"]]:raise ValueError("请选择包内引脚描述文件")
        rows=self._parse_pins(self.pack.read(name,relative=False),Path(name).suffix.lower())
        self.pins=rows;self.pin_source="Pack: "+name;self.rev+=1
        return {"rows":rows,"source":self.pin_source}

    @method("按引脚名或功能过滤引脚表")
    def pin_table(self,query:str=""):
        return {"rows":[p for p in self.pins if query.casefold() in (p["number"]+" "+p["name"]+" "+p["functions"]).casefold()],"source":self.pin_source,"rev":self.rev}

    @method("外设选择列表")
    def peripheral_options(self):return {"peripherals":[{"label":p["name"]+" · "+p["base"],"value":p["name"]} for p in self.model.peripherals] if self.model else []}

    @method("选择外设，更新寄存器表")
    def select_peripheral(self,name:str):
        m=self.registers_model()
        if name not in [p["name"] for p in m.peripherals]:raise ValueError("外设不存在")
        self.peripheral=name;self.register=next((k for k,v in m.registers.items() if v["peripheral"]==name),"");self.rev+=1
        return self.info()

    def values(self):return self.configured.setdefault((self.device,self.processor),{})

    def reg_value(self,r):return self.values().get(r["path"],{}).get("value",r["reset"])

    @staticmethod
    def editable(r):return writable(r["access"]) and not r["special"] and not any(f["special"] for f in r["fields"])

    @method("当前外设的寄存器及离线配置值")
    def registers(self,query:str=""):
        rows=[]
        if self.model:
            for r in self.model.registers.values():
                if r["peripheral"]!=self.peripheral or query.casefold() not in (r["path"]+" "+r["description"]).casefold():continue
                rows.append({k:r[k] for k in ("path","name","address","size","access","description")}|{
                    "reset":f"0x{r['reset']:0{(r['size']+3)//4}X}","value":f"0x{self.reg_value(r):0{(r['size']+3)//4}X}",
                    "readonly":not self.editable(r),"configured":r["path"] in self.values()})
        return {"rows":rows,"rev":self.rev}

    @method("选择寄存器以显示位域")
    def select_register(self,path:str):
        if path not in self.registers_model().registers:raise ValueError("寄存器不存在")
        self.register=path;self.rev+=1;return self.fields()

    @method("选中寄存器位域、访问权限和枚举")
    def fields(self):
        if not self.model or not self.register:return {"rows":[],"rev":self.rev}
        r=self.model.registers[self.register];value=self.reg_value(r)
        return {"register":self.register,"address":r["address"],"rows":[dict(f,path=self.register,bit_name=f["name"],bits=f"[{f['offset']+f['width']-1}:{f['offset']}]",
            value=str((value>>f["offset"])&((1<<f["width"])-1)),readonly=not(self.editable(r) and writable(f["access"])),
            enums="; ".join(e["value"]+"="+e["name"] for e in f["enum"])) for f in r["fields"]],"rev":self.rev}

    def _write(self,r,value,mask):
        if not self.editable(r):raise ValueError("只读或带特殊读写语义的寄存器仅支持浏览")
        value=number(value)
        if not 0<=value<(1<<r["size"]):raise ValueError("配置值超出寄存器位宽")
        allowed=0
        for f in r["fields"]:
            if writable(f["access"]):allowed|=((1<<f["width"])-1)<<f["offset"]
        if not r["fields"]:allowed=(1<<r["size"])-1
        if (value^self.reg_value(r))&~allowed:raise ValueError("不能修改只读位域或未描述的保留位")
        for f in r["fields"]:
            v=(value>>f["offset"])&((1<<f["width"])-1)
            if writable(f["access"]) and not f["min"]<=v<=f["max"]:raise ValueError("位域配置超出 writeConstraint 范围："+f["name"])
        previous=self.values().get(r["path"],{}).get("mask",0)
        self.values()[r["path"]]={"value":value,"mask":previous|(mask&allowed)};self.rev+=1
        return {"path":r["path"],"value":f"0x{value:X}","rev":self.rev}

    @method("编辑寄存器的离线配置值，不写入芯片")
    def set_register(self,path:str,field:str,value):
        if field!="value":raise ValueError("仅配置值可编辑")
        r=self.registers_model().registers.get(path)
        if r is None:raise ValueError("寄存器不存在")
        return self._write(r,value,(1<<r["size"])-1)

    @method("编辑位域的离线配置值，不写入芯片")
    def set_field(self,path:str,bit_name:str,field:str,value):
        if field!="value":raise ValueError("仅配置值可编辑")
        r=self.registers_model().registers.get(path)
        if r is None:raise ValueError("寄存器不存在")
        f=next((f for f in r["fields"] if f["name"]==bit_name),None)
        if f is None or not writable(f["access"]):raise ValueError("位域不存在或只读")
        v=number(value)
        if not 0<=v<(1<<f["width"]):raise ValueError("数值超出位域宽度")
        mask=((1<<f["width"])-1)<<f["offset"]
        return self._write(r,(self.reg_value(r)&~mask)|(v<<f["offset"]),mask)

    @method("当前器件配置清单")
    def configuration(self):
        if not self.pack:return {"rows":[],"count":0}
        rows=[]
        for path,v in self.values().items():
            if self.model and path in self.model.registers:
                r=self.model.registers[path];rows.append({"path":path,"address":r["address"],"size":r["size"],"access":r["access"],"value":f"0x{v['value']:X}","mask":f"0x{v['mask']:X}"})
        return {"rows":rows,"count":len(rows),"device":self.device,"processor":self.processor}

    @method("还原指定寄存器的离线配置为 SVD 复位值")
    def reset_register(self,path:str):
        if path not in self.registers_model().registers:raise ValueError("寄存器不存在")
        self.values().pop(path,None);self.rev+=1;return {"path":path}

    @method("导出离线寄存器配置 JSON 或 C 初始化代码")
    def export_configuration(self,path:str,format:str="json"):
        self.state()
        if not path.strip():raise ValueError("请填写导出路径")
        data=self.configuration()
        if format=="json":content=json.dumps(dict(data,pack=self.pack.meta,svd=self.svd_path),ensure_ascii=False,indent=2)
        elif format=="c":
            lines=["/* Offline configuration plan. Review against the reference manual before use. */","#include <stdint.h>","","void mcu_assistant_apply_config(void)","{"]
            for row in data["rows"]:
                size=row["size"]
                if size not in (8,16,32,64):raise ValueError("C 导出仅支持 8/16/32/64 位寄存器")
                ptr=f"(volatile uint{size}_t *)(uintptr_t){row['address']}ULL"
                lines.append("    /* "+row["path"].replace("*/","")+" */")
                lines.append("    {")
                lines.append(f"        volatile uint{size}_t *reg = {ptr};")
                value=row["value"]+"ULL";mask=row["mask"]+"ULL"
                lines.append(f"        *reg = (uint{size}_t)"+(f"{value};" if row["access"] in ("write-only","writeOnce") else f"((*reg & ~{mask}) | ({value} & {mask}));"))
                lines.append("    }")
            lines.append("}\n");content="\n".join(lines)
        else:raise ValueError("格式须为 json 或 c")
        out=Path(path).expanduser();out.parent.mkdir(parents=True,exist_ok=True)
        with out.open("x",encoding="utf-8") as file:file.write(content)
        return {"path":str(out.resolve()),"count":data["count"]}
