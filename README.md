# CS2 摸鱼切换器

一个纯 Win32 GUI 小工具。它通过 Valve/CS2 的 Game State Integration（GSI）接口监听本机玩家血量；玩家从存活变为死亡时，会立即：

- 切换到指定程序（未运行则启动），或
- 发送录制的快捷键（默认 `Alt + Tab`）。

程序使用 GSI 中的 SteamID 区分本机玩家与被观战的队友。死亡后队友的状态变化不会触发动作；下一回合检测到本机玩家复活时，会自动切换回 CS2。

## 构建

需要 Visual Studio 2022 的“使用 C++ 的桌面开发”工作负载和 CMake：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成文件位于 `build/Release/CSMoyu.exe`。

## 使用

1. 启动工具，点击“安装 CS2 监听配置…”。
2. 选择 CS2 安装目录内的 `game\bin\win64\cs2.exe`。
3. 如果 CS2 正在运行，请重启游戏，让 GSI 配置生效。
4. 选择“切换到程序”并浏览目标 `.exe`，或选择“发送快捷键”后点击快捷键框录制组合键。
5. 点击“开始监听”。状态显示“监听中”后即可进入游戏。

监听器只绑定 `127.0.0.1:3000`，不会接收局域网或互联网连接。配置保存在 `%LOCALAPPDATA%\CSMoyu\settings.ini`。

![](/doc_img/CSMoyu.png)

## 注意

- 只有在接收到 `health > 0` 后再次收到 `health == 0` 才触发，因此不会因重复 GSI 数据连续切换。
- 安装监听配置时会记录 `cs2.exe` 的路径；如果尚未记录，复活时会按进程名查找 CS2 窗口。
- 若目标程序以管理员身份运行，本工具也可能需要以管理员身份运行，Windows 才允许它激活窗口或发送按键。
- 端口 3000 被其他程序占用时，界面会显示监听失败。
