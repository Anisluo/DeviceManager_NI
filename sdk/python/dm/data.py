"""Read measurement runs back from the host database.

    from dm import data
    data.runs()[:5]                 # newest first: [{"id", "name", "kind", "status", "points", ...}]
    t = data.load(42)               # Table: t.columns, t.rows (list of dicts), t["vout"] (column)
    t.where(iload=0.01)["vout"]     # simple filtering
    t.to_pandas()                   # if pandas is installed

Read-only: the host is the only writer. Works outside the host too, given a
path: data.load(42, db="F:/61-NI/data/devicemanager.sqlite").
"""

import contextlib
import json
import os
import sqlite3

__all__ = ["db_path", "runs", "load", "Table", "export_parquet"]


def db_path():
    import _dmhost
    path = _dmhost.db_path()
    if not path:
        raise RuntimeError("no measurement database is open")
    return path


def _connect(db=None):
    """Read-only connection, closed when the with-block ends (sqlite3's own context manager doesn't close)."""
    path = db or db_path()
    return contextlib.closing(sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=5))


def runs(limit=100, db=None):
    with _connect(db) as c:
        cur = c.execute("SELECT id, name, kind, station, script, started, finished, status, points, meta "
                        "FROM runs ORDER BY id DESC LIMIT ?", (limit,))
        keys = [d[0] for d in cur.description]
        out = []
        for row in cur:
            r = dict(zip(keys, row))
            r["meta"] = json.loads(r["meta"] or "{}")
            out.append(r)
        return out


class Table:
    """Rows of one run, columns in recording order."""

    def __init__(self, run, columns, rows):
        self.run = run              # dict with id, name, kind, meta, ...
        self.columns = columns
        self.rows = rows            # list of dicts

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, column):
        return [r.get(column) for r in self.rows]

    def where(self, **equals):
        rows = [r for r in self.rows if all(r.get(k) == v for k, v in equals.items())]
        return Table(self.run, self.columns, rows)

    def unique(self, column):
        seen = []
        for v in self[column]:
            if v not in seen:
                seen.append(v)
        return seen

    def to_pandas(self):
        import pandas as pd
        return pd.DataFrame(self.rows, columns=self.columns)

    def __repr__(self):
        return f"<Table run {self.run.get('id')} '{self.run.get('name')}': {len(self.rows)} rows × {self.columns}>"


def load(run_id, db=None):
    with _connect(db) as c:
        cur = c.execute("SELECT id, name, kind, station, script, started, finished, status, columns, meta, points "
                        "FROM runs WHERE id = ?", (run_id,))
        row = cur.fetchone()
        if row is None:
            raise KeyError(f"no run {run_id}")
        keys = [d[0] for d in cur.description]
        run = dict(zip(keys, row))
        columns = json.loads(run.pop("columns") or "[]")
        run["meta"] = json.loads(run["meta"] or "{}")
        rows = [json.loads(d) for (d,) in c.execute("SELECT data FROM points WHERE run_id = ? ORDER BY seq", (run_id,))]
        return Table(run, columns, rows)


def _export_path(run, ext):
    import _dmhost
    folder = _dmhost.export_dir()
    os.makedirs(folder, exist_ok=True)
    safe = "".join(ch if ch.isalnum() or ch in "-_." else "_" for ch in run["name"])
    return os.path.join(folder, f"run{run['id']}_{safe}.{ext}")


def export_parquet(run_id, path=None):
    try:
        import pyarrow as pa
        import pyarrow.parquet as pq
    except ImportError:
        raise RuntimeError("Parquet 导出需要 pyarrow：在宿主所用的 Python 中执行 pip install pyarrow") from None
    t = load(run_id)
    table = pa.table({c: t[c] for c in t.columns})
    table = table.replace_schema_metadata({"run": json.dumps(t.run, ensure_ascii=False, default=str)})
    path = path or _export_path(t.run, "parquet")
    pq.write_table(table, path)
    import _dmhost
    _dmhost.file_exported(path)
    return path
