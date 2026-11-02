"""Strict 100k-sample FFT/analysis acceptance; nonzero exit on a budget violation."""
import argparse
import csv
import json
import platform
import sys
import tempfile
from pathlib import Path
from time import perf_counter

import numpy as np
import scipy

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT/"sdk/python"), str(Path(__file__).resolve().parents[1])]
from signal_analysis import SignalAnalysis, spectrum


def summary(values):
    return {"runs": len(values), "max_ms": max(values), "p95_ms": float(np.percentile(values,95)), "median_ms": float(np.median(values)), "samples_ms": values}


def run():
    parser=argparse.ArgumentParser()
    parser.add_argument("--report", default=str(ROOT/"build/performance/signal_analysis_fft.json"))
    args=parser.parse_args()
    count=100000; fs=200000
    t=np.arange(count)/fs
    raw=np.sin(2*np.pi*1000*t)+.5*np.sin(2*np.pi*20000*t)
    report={"samples":count, "sample_rate":fs, "fft_points":count, "budget_ms":1000,
            "environment":{"python":platform.python_version(), "numpy":np.__version__, "scipy":scipy.__version__, "platform":platform.platform()},
            "scope":"Library import, plugin creation and CSV file I/O excluded; first measured 100k FFT included. Full pipeline includes column conversion, time checks, filtering, two FFTs, chart reduction and both ABI JSON replies."}
    timings=[]; pipeline=[]
    with tempfile.TemporaryDirectory() as tmp:
        path=Path(tmp)/"100k.csv"
        with path.open("w",newline="",encoding="utf-8") as f:
            writer=csv.writer(f);writer.writerow(["time_s","signal_V"]);writer.writerows(zip(t,raw))
        plugin=SignalAnalysis({})
        start=perf_counter();plugin.load_csv(str(path));report["csv_load_ms"]=(perf_counter()-start)*1000
        # Configure state directly so the first 100k FFT is part of the measurements.
        plugin.sampling.update(data_column=1,time_column=0,fs_mode="time",sample_rate=fs)
        config={"filter_options":{"family":"butter","kind":"lowpass","cutoff1":5000,"order":4,"phase":"zero"},
                "fft_options":{"nfft":0,"window":"hann","detrend":"constant","scale":"psd_db"}}
        for _ in range(10):
            start=perf_counter();f,a,p=spectrum(raw,fs,"hann",0,"constant");timings.append((perf_counter()-start)*1000)
            assert len(f)==50001 and abs(f[np.argmax(a)]-1000)<1e-6
            assert abs(a[500]-1)<1e-8 and abs(np.sum(p)*(fs/count)-.625)<1e-8
            start=perf_counter()
            rc,reply=plugin._dm_invoke_json("analyze",json.dumps(config))
            rc2,graphs=plugin._dm_invoke_json("result","{}")
            elapsed=(perf_counter()-start)*1000
            assert rc==0 and rc2==0,reply
            pipeline.append(elapsed)
            result=json.loads(reply)
            assert result["count"]==count and abs(result["sample_rate"]-fs)<1e-3
            assert len(plugin.full["raw"])==count and len(plugin.full["frequency"])==50001
        report["fft"]=summary(timings);report["full_pipeline"]=summary(pipeline)
        report["all_filters_ms"]={}
        for family in ("none","butter","cheby1","cheby2","ellip","bessel","fir","notch","moving_average","median","savgol"):
            c=dict(config["filter_options"],family=family)
            start=perf_counter()
            rc,reply=plugin._dm_invoke_json("analyze",json.dumps({"filter_options":c}))
            rc2,graphs=plugin._dm_invoke_json("result","{}")
            elapsed=(perf_counter()-start)*1000
            assert rc==0 and rc2==0,reply
            report["all_filters_ms"][family]=elapsed
        report["filter_parameters"]={"iir_order":4,"fir_or_smoothing_length":31,"savgol_polyorder":3,"notch_q":30,"phase":"zero"}
    report["passed"]=max(timings+pipeline+list(report["all_filters_ms"].values()))<=1000
    output=Path(args.report);output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps({"passed":report["passed"],"fft":report["fft"],"full_pipeline":report["full_pipeline"],"all_filters_ms":report["all_filters_ms"],"report":str(output)},ensure_ascii=False))
    return 0 if report["passed"] else 1


if __name__=="__main__":raise SystemExit(run())
