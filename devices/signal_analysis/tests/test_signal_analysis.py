import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT/"sdk/python"), str(Path(__file__).resolve().parents[1])]
from signal_analysis import SignalAnalysis, filter_signal, spectrum, thin_indices


class Analysis(unittest.TestCase):
    def setUp(self):
        self.p = SignalAnalysis({})
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def csv(self, content, encoding="utf-8-sig"):
        file = self.dir/"原始数据.csv"
        file.write_text(content, encoding=encoding)
        return str(file)

    def test_demo_and_full_precision_export(self):
        self.assertEqual(self.p.result()["count"], 4096)
        file = self.dir/"result.csv"
        self.p.export(str(file))
        with file.open(encoding="utf-8-sig", newline="") as stream:
            rows = list(csv.reader(stream))
        self.assertEqual(len(rows), 4097)
        np.testing.assert_allclose(np.array(rows[1:], dtype=float)[:, 2], self.p.full["filtered"])
        with self.assertRaises(FileExistsError): self.p.export(str(file))
        self.p.export(str(self.dir/"settings.json"), "settings")
        self.assertIn("filter", json.loads((self.dir/"settings.json").read_text(encoding="utf-8")))

    def test_csv_columns_skip_header_and_time_inference(self):
        path = self.csv("instrument export\n时间;电压;注释\n"+"".join(f"{i};{i*.1};sample\n" for i in range(64)), "gb18030")
        self.p.load_csv(path, skip_rows=1, encoding="gb18030")
        self.assertEqual(self.p.info()["samples"], 64)
        self.p.configure_sampling(data_column=1, time_column=0, fs_mode="time", time_unit="ms", start=4, count=32)
        self.assertAlmostEqual(self.p.result()["sample_rate"], 1000)
        self.assertAlmostEqual(self.p.full["raw"][0], .4)
        self.assertEqual(self.p.result()["count"], 32)
        self.assertEqual(self.p.filter["family"], "none")

    def test_headerless_single_column_and_scaling(self):
        self.p.load_csv(self.csv("\n".join(str(i) for i in range(32))), header="no")
        self.p.configure_sampling(data_column=0, time_column=-1, sample_rate=1000, count=16, gain=2, offset=-1)
        np.testing.assert_equal(self.p.full["raw"], np.arange(16)*2-1)

    def test_time_jitter_and_manual_fs_mismatch_rejected(self):
        path = self.csv("t,y\n"+"".join(f"{i*.001},{i}\n" for i in range(32)))
        self.p.load_csv(path)
        with self.assertRaises(ValueError): self.p.configure_sampling(data_column=1, time_column=0, sample_rate=2000)
        self.p.rows[7][0] = "0.0075"
        with self.assertRaisesRegex(ValueError, "0.1%"):
            self.p.configure_sampling(data_column=1, time_column=0, fs_mode="time")
        self.p.rows[7][0] = "0.006"
        with self.assertRaisesRegex(ValueError, "严格递增"):
            self.p.configure_sampling(data_column=1, time_column=0, fs_mode="time")

    def test_invalid_csv_does_not_destroy_previous_dataset(self):
        old = self.p.full
        with self.assertRaises(ValueError): self.p.load_csv(self.csv("a,b\n1,2,3\n"))
        self.assertIs(self.p.full, old)
        path = self.csv("v\n"+"\n".join(["1"]*12+["bad"]+["1"]*12))
        self.p.load_csv(path)
        with self.assertRaisesRegex(ValueError, "14"):
            self.p.configure_sampling(data_column=0)
        self.assertIsNone(self.p.full)

    def test_fft_peak_amplitude_dc_and_nyquist(self):
        fs = 4096; t = np.arange(fs)/fs
        freq, amp, psd = spectrum(2+3*np.cos(2*np.pi*256*t), fs, "boxcar", 0, "none")
        self.assertAlmostEqual(amp[0], 2)
        self.assertAlmostEqual(amp[256], 3)
        self.assertAlmostEqual(psd.sum()*(freq[1]-freq[0]), 8.5)
        _, amp, _ = spectrum(2*np.cos(np.pi*np.arange(64)), 64, "boxcar", 0, "none")
        self.assertAlmostEqual(amp[-1], 2)  # Nyquist is not doubled.
        _, amp, _ = spectrum(3*np.cos(2*np.pi*31*np.arange(63)/63), 63, "boxcar", 0, "none")
        self.assertAlmostEqual(amp[-1], 3)  # Last positive bin of odd FFT is doubled.

    def test_default_spectrum_is_power_density_db(self):
        self.assertEqual(self.p.fft["scale"], "psd_db")
        chart = self.p.result()["spectrum"]
        self.assertIn("²/Hz", chart["y_unit"])
        np.testing.assert_allclose(chart["series"][0]["values"], 10*np.log10(np.maximum(self.p.full["raw_psd"],1e-30)))
        np.testing.assert_allclose(chart["series"][1]["values"], 10*np.log10(np.maximum(self.p.full["psd"],1e-30)))

    def test_windows_gain_and_zero_padding(self):
        t = np.arange(4096)/4096
        for window in ("hann", "hamming", "blackman", "blackmanharris", "flattop"):
            _, amp, _ = spectrum(2*np.sin(2*np.pi*128*t), 4096, window, 8192, "none")
            self.assertAlmostEqual(amp[256], 2, places=8)

    def test_iir_families_and_fir_remove_high_tone(self):
        fs = 4096; t = np.arange(8192)/fs
        x = np.sin(2*np.pi*64*t)+np.sin(2*np.pi*1400*t)
        for family in ("butter", "cheby1", "cheby2", "ellip", "bessel", "fir"):
            c = dict(self.p.filter, family=family, cutoff1=300, length=101)
            y, f, gain, note = filter_signal(x, fs, c)
            middle = y[1024:-1024]; tm = t[1024:-1024]
            low = abs(np.mean(middle*np.exp(-2j*np.pi*64*tm)))*2
            high = abs(np.mean(middle*np.exp(-2j*np.pi*1400*tm)))*2
            self.assertGreater(low, .5, family)
            self.assertLess(high, .02, family)
            self.assertTrue(np.isfinite(gain).all())

    def test_high_band_and_stop_filters(self):
        fs=4096; t=np.arange(16384)/fs
        x=sum(np.sin(2*np.pi*f*t) for f in (64,512,1400))
        for kind, kept, removed in (("highpass", 1400,64), ("bandpass",512,1400), ("bandstop",1400,512)):
            c=dict(self.p.filter, kind=kind, cutoff1=300, cutoff2=800)
            y,*_=filter_signal(x,fs,c)
            mid=y[2048:-2048]; tm=t[2048:-2048]
            strength=lambda f: abs(np.mean(mid*np.exp(-2j*np.pi*f*tm)))*2
            self.assertGreater(strength(kept),.8)
            self.assertLess(strength(removed),.03)

    def test_notch_and_smoothers(self):
        fs=4096; t=np.arange(16384)/fs
        x=np.sin(2*np.pi*50*t)+np.sin(2*np.pi*512*t)
        y,*_=filter_signal(x,fs,dict(self.p.filter,family="notch",cutoff1=512,q=20))
        self.assertLess(abs(np.mean(y[2048:-2048]*np.exp(-2j*np.pi*512*t[2048:-2048])))*2,.005)
        impulse=np.zeros(1024);impulse[512]=10
        med,*_=filter_signal(impulse,fs,dict(self.p.filter,family="median"))
        self.assertEqual(med.max(),0)
        for family in ("moving_average","savgol"):
            y,*_=filter_signal(x,fs,dict(self.p.filter,family=family))
            self.assertTrue(np.isfinite(y).all())

    def test_zero_phase_response_is_squared(self):
        x=np.sin(np.arange(4096)*.1)
        _,f,a,_=filter_signal(x,10000,dict(self.p.filter,phase="causal"))
        _,f,b,_=filter_signal(x,10000,dict(self.p.filter,phase="zero"))
        np.testing.assert_allclose(b,a*a)

    def test_invalid_processing_keeps_last_success(self):
        old=self.p.full
        for config in ({"cutoff1":5000},{"kind":"bandpass","cutoff2":100},{"family":"median","length":4095},{"family":"savgol","polyorder":40}):
            with self.assertRaises(ValueError): self.p.analyze(config)
            self.assertIs(self.p.full,old)
        with self.assertRaises(ValueError): self.p.analyze(fft_options={"nfft":100})
        with self.assertRaises(ValueError): self.p.analyze(fft_options={"bad":1})

    def test_thinning_preserves_spikes_on_both_curves(self):
        a=np.zeros(100000); b=a.copy(); a[12345]=99;b[54321]=-100
        idx=thin_indices([a,b],6000)
        self.assertIn(12345,idx);self.assertIn(54321,idx)
        self.assertLessEqual(len(idx),6002)

    def test_page_defaults_call_real_methods(self):
        page=json.loads((Path(__file__).resolve().parents[1]/"page.json").read_text(encoding="utf-8"))
        tab=page["tabs"][1]
        widgets=[w for s in tab["sections"] for w in s["widgets"]]
        form={w["id"]:w["value"] for w in widgets if "value" in w}
        action=next(w["action"] for w in widgets if w.get("action",{}).get("method")=="analyze")
        def sub(v):
            if isinstance(v,str) and v.startswith("$form."):return form[v[6:]]
            if isinstance(v,dict):return {k:sub(x) for k,x in v.items()}
            return v
        rc,text=self.p._dm_invoke_json(action["method"],json.dumps(sub(action["args"])))
        self.assertEqual(rc,0,text)


if __name__ == "__main__": unittest.main()
