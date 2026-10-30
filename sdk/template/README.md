# my_plugin

插件模板。使用方法：

1. 复制本目录到 `devices/<你的插件id>/`
2. 在 `plugin.json`、`CMakeLists.txt`、`src/my_plugin.cpp` 中把 `my_plugin` 改成新 id
3. 重新运行 CMake 配置并编译；宿主点"重新扫描插件"即可看到
4. 在工位文件里添加设备：`{ "name": "dev1", "plugin": "<id>", "resource": "..." }`

接口和 page.json 说明见 `sdk/README.md`。
