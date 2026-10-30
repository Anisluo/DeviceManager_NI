# my_py_plugin（Python 插件模板）

1. 复制本目录到 `devices/<你的插件id>/`，把 `plugin.json` 里的 `id`/`entry` 和 `.py` 文件名、类名改成自己的
2. 不需要编译：宿主点"重新扫描插件"即可看到；之后每次保存 `.py` 文件都会自动重新加载
3. 在工位文件里添加设备：`{ "name": "dev1", "plugin": "<id>", "resource": "..." }`

`@method` 标记的方法按 JSON 参数名以关键字参数调用；参数不匹配时宿主收到 `DM_E_BAD_ARGS`，方法内抛出 `ValueError` 也是如此。详见 `sdk/README.md`。
