import json
from pathlib import Path


def text(id,label,placeholder=""):
    return dict(type="text",id=id,label=label,value="",placeholder=placeholder,mono=True)


def button(label,method,args=None,**extra):
    return dict({"type":"button","label":label,"fit":True,"style":"primary","action":{"method":method,"args":args or {}}},**extra)


def selector(id,label,method,field,init=None):
    w=dict(type="combo",id=id,label=label,options_from={"method":method,"field":field})
    if init:w["init"]={"method":"info","field":init}
    return w


def section(title,widgets,subtitle="",span=12):return dict(title=title,widgets=widgets,subtitle=subtitle,span=span)


def read(label,source,field):return dict(type="readout",label=label,source=source,field=field)


def table(source,fields,rows="rows"):
    return {"type":"table","source":source,"rows_field":rows,"columns":[{"field":f,"label":label} for f,label in fields]}


def main():
    refresh=["info","algorithms","pins","registers","fields","config"]
    regs={"type":"edit_table","source":"registers","rows_field":"rows","rev_field":"rev","key":["path"],"write":"set_register","height":410,"also":["fields","config"],
          "columns":[{"field":f,"label":label,"width":width,**extra} for f,label,width,extra in [
              ("name","寄存器",160,{}),("address","地址",130,{}),("size","位宽",60,{}),("access","权限",110,{}),
              ("reset","复位值",120,{}),("value","离线配置值",140,{"editor":"text","mono":True,"readonly_if":"readonly"}),
              ("description","描述",300,{})]],
          "row_actions":[{"label":"查看位域","method":"select_register"},{"label":"还原复位值","method":"reset_register"}]}
    fields={"type":"edit_table","source":"fields","rows_field":"rows","rev_field":"rev","key":["path","bit_name"],"write":"set_field","height":320,"also":["registers","config"],
            "columns":[{"field":f,"label":label,"width":width,**extra} for f,label,width,extra in [
                ("bit_name","位域",150,{}),("bits","位位置",80,{}),("access","权限",110,{}),
                ("value","离线值",100,{"editor":"text","readonly_if":"readonly"}),("enums","枚举",330,{}),("description","描述",280,{})]]}
    sources=[{"id":"info","method":"info","interval_ms":2000},{"id":"algorithms","method":"algorithms","interval_ms":5000},
             {"id":"pins","method":"pin_table","interval_ms":3000},{"id":"registers","method":"registers","interval_ms":3000},
             {"id":"fields","method":"fields","interval_ms":3000},{"id":"config","method":"configuration","interval_ms":3000}]
    page={"title":"MCU助手","subtitle":"CMSIS-Pack · 器件 / 封装 · 烧录算法 · 引脚功能 · SVD 寄存器离线配置","columns":12,"sources":sources,"tabs":[
        {"title":"Pack与器件","sections":[
            section("导入 Pack",[text("pack_path","文件路径","C:\\Packs\\Vendor.Device_DFP.1.0.0.pack"),
                button("导入 Pack","import_pack",{"path":"$form.pack_path"},refresh=refresh,reload_page=True),
                button("载入教学示例","demo",refresh=refresh,reload_page=True)],"支持 .pack 和已解包的 .pdsc；仅解析元数据，不执行包内脚本或算法。"),
            section("选择 MCU",[text("device_query","型号筛选","例如 STM32L072"),button("筛选型号","filter_devices",{"query":"$form.device_query"},reload_page=True),
                selector("device_choice","器件型号","device_options","devices","device"),button("打开器件","select_device",{"name":"$form.device_choice"},refresh=refresh,reload_page=True),
                selector("processor_choice","处理器","processor_options","processors","processor"),button("切换内核","select_processor",{"name":"$form.processor_choice"},refresh=refresh,reload_page=True)],
                "封装不同的型号/variant 分开选择。多核 MCU 可分别选择处理器和对应 SVD。"),
            section("器件信息",[read(label,"info",field) for label,field in [("Pack","pack"),("当前器件","device"),("内核","core"),("封装引脚数","pin_count"),("GPIO 数量","gpio_count"),("SVD 状态","svd_status"),("寄存器数","registers")]],
                "封装引脚数来自 PDSC feature，GPIO 数量不是封装引脚数量。缺失的数据明确标注，不依据型号后缀猜测。"),
            section("内存布局",[table("info",[("name","名称"),("id","标识"),("start_hex","起始地址"),("bytes","字节数"),("access","权限")],"memory")]),
            section("器件功能",[table("info",[("type","类别"),("n","数量/最小"),("m","位宽/最大"),("name","说明")],"features")])]},
        {"title":"烧录算法","sections":[
            section("算法文件与覆盖范围",[table("algorithms",[("name","算法文件"),("start","Flash 起始"),("bytes","覆盖字节"),("default","默认"),("style","格式"),("exists","文件存在")])],
                "列出 PDSC 继承后的算法及 Flash 区间。FLM 符号值可能是节内偏移，不能直接当作运行时入口；本插件不执行烧录。"),
            section("FLM 文件分析",[selector("algorithm","算法文件","algorithm_options","algorithms"),
                button("解析 FLM","inspect_algorithm",{"name":"$form.algorithm"},publish="flm"),
                read("Flash 型号","flm","device_name"),read("Flash 起始","flm","flash_start"),read("Flash 字节","flm","flash_bytes"),read("页字节","flm","page_bytes"),
                table("flm",[("name","入口符号"),("address","ELF 符号值"),("size","代码字节")],"functions"),
                table("flm",[("size","扇区字节"),("offset","Flash 内偏移")],"sectors")]),
            section("导出算法文件",[text("flm_export_path","输出路径","C:\\Export\\device.FLM"),button("导出 FLM","export_algorithm",{"name":"$form.algorithm","path":"$form.flm_export_path"},publish="exported"),read("已导出","exported","path")],"原样导出选中的算法文件，目标文件须不存在。") ]},
        {"title":"引脚功能","sections":[
            section("数据来源",[read("封装引脚数","info","pin_count"),read("逐引脚来源","info","pin_source"),read("引脚表条目","info","pin_rows")],
                "CMSIS-Pack 不保证提供管脚编号和复用映射。没有数据时不将 GPIO 寄存器列表冒充封装引脚表。请选择与当前器件/封装匹配的厂商引脚数据。"),
            section("读取包内引脚文件",[selector("pack_pin_file","包内文件","pin_file_options","files"),
                button("读取引脚文件","import_pack_pins",{"name":"$form.pack_pin_file"},refresh=["pins","info"])],"识别厂商 MCU XML 的 Pin/Signal 结构或标准列 CSV；不支持的格式会明确报错。"),
            section("补充引脚表",[text("pins_path","文件路径","CSV / 厂商 MCU XML"),button("导入引脚表","import_pins",{"path":"$form.pins_path"},refresh=["pins","info"])],
                "CSV 列：number,name,functions，可选 type；XML 支持 Pin 的 Position/Name/Type 和 Signal Name。导入表不会自动写 GPIO 复用寄存器。"),
            section("引脚与复用功能",[table("pins",[("number","管脚编号"),("name","引脚名称"),("type","类型"),("functions","功能 / 复用")])])]},
        {"title":"寄存器配置","sections":[
            section("SVD 外设",[read("SVD 状态","info","svd_status"),selector("peripheral_choice","选择外设","peripheral_options","peripherals","peripheral"),
                button("打开外设","select_peripheral",{"name":"$form.peripheral_choice"},refresh=["registers","fields","info"])],
                "显示 SVD 描述及离线值。只读寄存器、保留位、特殊读写副作用不允许修改；不会读写真实 MCU。"),
            section("寄存器列表",[regs],"双击“离线配置值”输入十六进制/十进制。选中行后点击“查看位域”，或右键使用行操作。"),
            section("位域配置",[read("当前寄存器","fields","register"),fields],"显示位位置、访问属性和枚举。可修改普通可写位域，修改会同步到寄存器配置值。") ]},
        {"title":"配置导出","sections":[
            section("已配置寄存器",[read("配置数量","config","count"),table("config",[("path","寄存器"),("address","地址"),("value","配置值"),("mask","配置掩码")])],"仅导出明确编辑过的寄存器。未配置项不生成写操作。"),
            section("导出离线初始化方案",[text("config_path","输出路径","C:\\Export\\mcu_config.json"),
                {"type":"combo","id":"config_format","label":"格式","options":["json","c"],"value":"json"},
                button("导出配置","export_configuration",{"path":"$form.config_path","format":"$form.config_format"},publish="exported"),read("已导出","exported","path")],
                "JSON 保留器件、SVD、地址、值和掩码；C 为待审阅的初始化代码。读写寄存器按掩码读改写，只写寄存器直接写值；不自动执行，不覆盖已有文件。") ]}
    ]}
    (Path(__file__).resolve().parents[1]/"page.json").write_text(json.dumps(page,ensure_ascii=False,indent=1)+"\n",encoding="utf-8")


if __name__=="__main__":main()
