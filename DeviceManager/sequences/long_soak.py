"""Slow soak: log smu1 ch0 every 0.5 s until stopped (try the Stop button)."""

smu = device("smu1")
t0 = time.time()
n = 0
while True:
    m = smu.measure(channel=0)
    record(t=round(time.time() - t0, 3), current=m["current"], voltage=m["voltage"])
    n += 1
    progress((n % 20) / 20, f"{n} 个点")
    sleep(0.5)
