import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from zipfile import ZipFile

ROOT=Path(__file__).resolve().parents[3]
PLUGIN=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/"sdk/python"),str(PLUGIN)]
from pack_model import Pack,inspect_flm
from svd_model import Svd
from mcu_assistant import McuAssistant


def elf_fixture():
    flash=bytearray(184);struct.pack_into("<H",flash,0,0x101);flash[2:11]=b"TestFlash"
    struct.pack_into("<HIIII",flash,130,1,0x08000000,65536,256,0)
    flash[148]=0xFF;struct.pack_into("<II",flash,152,10,500)
    struct.pack_into("<II",flash,160,1024,0);struct.pack_into("<II",flash,168,0xFFFFFFFF,0xFFFFFFFF)
    names=b"\0FlashDevice\0Init\0ProgramPage\0"
    sym=bytes(16)+struct.pack("<IIIBBH",1,0,len(flash),0x11,0,1)+struct.pack("<IIIBBH",13,1,12,0x12,0,1)+struct.pack("<IIIBBH",18,13,20,0x12,0,1)
    base=52+40*4;off_names=base+len(flash);off_sym=off_names+len(names)
    header=b"\x7fELF\x01\x01\x01"+bytes(9)+struct.pack("<HHIIIIIHHHHHH",1,40,1,0,0,52,0,52,0,0,40,4,0)
    sections=bytes(40)+struct.pack("<IIIIIIIIII",0,1,0,0,base,len(flash),0,0,4,0)+struct.pack("<IIIIIIIIII",0,3,0,0,off_names,len(names),0,0,1,0)+struct.pack("<IIIIIIIIII",0,2,0,0,off_sym,len(sym),2,0,4,16)
    return header+sections+flash+names+sym


class Assistant(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.dir=Path(self.tmp.name)
        self.p=McuAssistant({});self.addCleanup(self.p.close)

    def test_demo_pin_count_fields_and_offline_config(self):
        info=self.p.demo();self.assertEqual(info["pin_count"],"8 (QFN)");self.assertEqual(len(self.p.pin_table()["rows"]),8)
        self.p.set_field("GPIOA.MODER","MODE2","value","2")
        row=self.p.configuration()["rows"][0]
        self.assertEqual(row["value"],"0x20");self.assertEqual(row["mask"],"0x30")
        self.assertEqual(self.p.fields()["rows"][2]["value"],"2")
        self.assertTrue(self.p.fields()["rows"][0]["enum"])
        with self.assertRaises(ValueError):self.p.set_field("GPIOA.MODER","MODE2","value","4")
        with self.assertRaises(ValueError):self.p.set_register("GPIOA.IDR","value","1")
        with self.assertRaises(ValueError):self.p.set_register("GPIOA.MODER","value","0x10000")
        self.p.reset_register("GPIOA.MODER");self.assertEqual(self.p.configuration()["count"],0)

    def test_pdsc_inheritance_variants_memory_and_processors(self):
        pdsc=self.dir/"test.pdsc"
        pdsc.write_text('''<package><name>Test</name><vendor>V</vendor><devices><family Dfamily="F" Dvendor="Vendor">
          <processor Pname="M4" Dcore="Cortex-M4"/><processor Pname="M0" Dcore="Cortex-M0"/>
          <memory name="FLASH" start="0x08000000" size="0x10000"/><feature type="QFP" n="100"/>
          <subFamily DsubFamily="S"><device Dname="Chip"><memory name="FLASH" size="0x20000"/><feature type="QFP" n="64"/>
          <variant Dvariant="Chip32"><feature type="QFP" count="32"/></variant></device></subFamily>
        </family></devices></package>''',encoding="utf-8")
        self.p.import_pack(str(pdsc));self.assertEqual(self.p.info()["pin_count"],"64 (QFP)")
        self.assertEqual(self.p.info()["memory"][0]["start_hex"],"0x08000000")
        self.assertEqual(self.p.info()["memory"][0]["bytes"],0x20000)
        self.p.select_device("Chip32");self.assertEqual(self.p.info()["pin_count"],"32 (QFP)")
        self.p.select_processor("M0");self.assertEqual(self.p.info()["core"],"Cortex-M0")
        self.assertIn("未提供",self.p.info()["svd_status"])

    def test_archive_and_unsafe_references(self):
        pack=self.dir/"demo.pack"
        with ZipFile(pack,"w") as z:
            z.write(PLUGIN/"examples/DemoMCU.pdsc","DemoMCU.pdsc");z.write(PLUGIN/"examples/DemoMCU.svd","DemoMCU.svd")
        self.p.import_pack(str(pack));self.assertEqual(self.p.info()["registers"],3)
        previous=self.p.pack
        bad=self.dir/"bad.pack"
        with ZipFile(bad,"w") as z:z.writestr("../escape.pdsc","<package/>")
        with self.assertRaises(ValueError):self.p.import_pack(str(bad))
        self.assertIs(self.p.pack,previous)
        with self.assertRaises(ValueError):previous.read("../secret")
        bad_xml=self.dir/"bad.pdsc";bad_xml.write_text('<!DOCTYPE x [<!ENTITY a "bad">]><package/>')
        with self.assertRaises(ValueError):Pack(str(bad_xml))

    def test_svd_derived_clusters_arrays_and_access(self):
        data=b'''<device><name>T</name><width>32</width><size>32</size><access>read-write</access><resetValue>3</resetValue><peripherals>
          <peripheral><name>P</name><baseAddress>0x40000000</baseAddress><registers>
            <register><name>CTRL</name><addressOffset>0</addressOffset><fields><field><name>MODE</name><bitOffset>0</bitOffset><bitWidth>2</bitWidth></field></fields></register>
            <register derivedFrom="CTRL"><name>COPY</name><addressOffset>4</addressOffset></register>
            <cluster><dim>2</dim><dimIncrement>32</dimIncrement><name>CH%s</name><addressOffset>16</addressOffset>
              <register><dim>2</dim><dimIncrement>4</dimIncrement><name>DATA%s</name><addressOffset>8</addressOffset><access>read-only</access></register>
            </cluster>
          </registers></peripheral>
          <peripheral derivedFrom="P"><name>Q</name><baseAddress>0x50000000</baseAddress></peripheral>
        </peripherals></device>'''
        m=Svd(data)
        self.assertEqual(len(m.registers),12)
        self.assertEqual(m.registers["Q.COPY"]["address_value"],0x50000004)
        self.assertEqual(m.registers["Q.CH1.DATA1"]["address_value"],0x5000003C)
        self.assertEqual(m.registers["P.COPY"]["fields"][0]["name"],"MODE")
        self.assertEqual(m.registers["P.CH0.DATA0"]["access"],"read-only")

    def test_svd_rejects_unresolved_or_cyclic_inheritance(self):
        for node in ('<peripheral derivedFrom="Missing"><name>P</name><baseAddress>0</baseAddress></peripheral>',
                     '<peripheral derivedFrom="P"><name>P</name><baseAddress>0</baseAddress></peripheral>'):
            with self.assertRaises(ValueError):Svd(("<device><peripherals>"+node+"</peripherals></device>").encode())

    def test_svd_enum_and_field_inheritance(self):
        m=Svd(b'''<device><peripherals><peripheral><name>P</name><baseAddress>0</baseAddress><registers><register><name>R</name><addressOffset>0</addressOffset><fields>
        <field><name>A</name><bitOffset>0</bitOffset><bitWidth>2</bitWidth><enumeratedValues><name>E</name><enumeratedValue><name>On</name><value>1</value></enumeratedValue></enumeratedValues></field>
        <field derivedFrom="A"><name>B</name><bitOffset>2</bitOffset></field>
        <field><name>C</name><bitOffset>4</bitOffset><bitWidth>2</bitWidth><enumeratedValues derivedFrom="P.R.A.E"/></field>
        </fields></register></registers></peripheral></peripherals></device>''')
        fields=m.registers["P.R"]["fields"]
        self.assertEqual(fields[1]["width"],2);self.assertEqual(fields[2]["enum"][0]["name"],"On")

    def test_special_write_semantics_blocked(self):
        self.p.demo();r=self.p.model.registers["GPIOA.ODR"]
        r["fields"][0]["special"]="oneToClear"
        with self.assertRaises(ValueError):self.p.set_register("GPIOA.ODR","value","1")
        self.assertEqual(self.p.configuration()["count"],0)

    def test_flm_symbols_flash_device_and_truncation(self):
        data=elf_fixture();result=inspect_flm(data)
        self.assertEqual(result["device_name"],"TestFlash")
        self.assertEqual(result["flash_start"],"0x08000000");self.assertEqual(result["page_bytes"],256)
        self.assertEqual(result["sectors"],[{"size":1024,"offset":"0x00000000"}])
        self.assertEqual([r["name"] for r in result["functions"]],["Init","ProgramPage"])
        for broken in (b"invalid",data[:60],data[:-20]):
            with self.assertRaises(ValueError):inspect_flm(broken)

    def test_pin_import_namespace_and_csv(self):
        self.p.demo();file=self.dir/"pins.xml"
        file.write_text('<Mcu xmlns="vendor"><Pin Position="1" Name="PA0" Type="I/O"><Signal Name="USART_TX"/><Signal Name="ADC_IN0"/></Pin></Mcu>')
        self.p.import_pins(str(file));row=self.p.pin_table("USART")["rows"][0]
        self.assertEqual(row["number"],"1");self.assertIn("ADC_IN0",row["functions"])

    def test_configuration_exports_only_explicit_values(self):
        self.p.demo();self.p.set_field("GPIOA.MODER","MODE0","value","1")
        j=self.dir/"cfg.json";c=self.dir/"cfg.c"
        self.p.export_configuration(str(j));self.p.export_configuration(str(c),"c")
        data=json.loads(j.read_text(encoding="utf-8"));code=c.read_text(encoding="utf-8")
        self.assertEqual(data["rows"][0]["mask"],"0x3")
        self.assertIn("0x40020000ULL",code);self.assertIn("~0x3ULL",code)
        self.assertNotIn("GPIOA.IDR",code)
        with self.assertRaises(FileExistsError):self.p.export_configuration(str(j))

    def test_page_methods_exist(self):
        page=json.loads((PLUGIN/"page.json").read_text(encoding="utf-8"));known=set(self.p._dm_methods())
        def names(n):
            if isinstance(n,list):
                for v in n:yield from names(v)
            elif isinstance(n,dict):
                for k,v in n.items():
                    if k in ("method","write","undo","delete_method","select","remove") and isinstance(v,str):yield v
                    else:yield from names(v)
        self.assertEqual(set(names(page))-known,set())

    def test_real_stm32l0_pack_when_available(self):
        file=Path(os.environ.get("MCU_ASSISTANT_TEST_PACK",r"F:/66-TDC/Keil.STM32L0xx_DFP.2.2.0.pack"))
        if not file.is_file():self.skipTest("Set MCU_ASSISTANT_TEST_PACK for a real vendor-pack check")
        info=self.p.import_pack(str(file))
        self.assertEqual(info["device"],"STM32L010C6Tx");self.assertEqual(info["pin_count"],"48 (QFP)")
        self.assertGreater(info["registers"],300)
        self.assertEqual(self.p.pin_table()["rows"],[])
        main=self.p.algorithms()["rows"][0];self.assertTrue(main["default"])
        flm=self.p.inspect_algorithm(main["name"]);self.assertEqual(flm["flash_start"],"0x08000000")
        self.p.select_device("STM32L072CBTx")
        self.assertIsNotNone(self.p.model,self.p.svd_error)
        b=self.p.model.registers["GPIOB.MODER"];c=self.p.model.registers["GPIOC.MODER"]
        self.assertEqual(c["address_value"]-b["address_value"],0x400)


if __name__=="__main__":unittest.main()
