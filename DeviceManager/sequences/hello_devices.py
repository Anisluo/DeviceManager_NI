"""Smallest possible sequence: list devices and read each one once."""

for name in devices():
    dev = device(name)
    names = [m["name"] for m in dev.methods()]
    print(f"{name:6s} methods: {', '.join(names)}")

print("smu1 ch0:", device("smu1").measure(channel=0))
print("dmm1    :", device("dmm1").read())
