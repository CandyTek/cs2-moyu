# CS2 摸鱼切换器

一个纯 Win32 GUI 小工具。它通过 Valve/CS2 的 Game State Integration（GSI）接口监听本机玩家血量；玩家从存活变为死亡时，会立即：

- 切换到指定程序（未运行则启动），或
- 发送录制的快捷键（默认 `Alt + Tab`）。

界面还提供“暂停音乐播放器”和“暂停网页视频”两个复选框。启用后，玩家死亡时会先发送明确的“暂停”媒体命令（不会把已暂停的内容重新播放），再执行切换动作。网页视频目前识别浏览器活动标签页标题中的哔哩哔哩、抖音和西瓜视频；音乐播放器支持网易云音乐、QQ 音乐、酷狗、酷我、Spotify、Windows 媒体播放器、foobar2000 和 AIMP。

程序使用 GSI 中的 SteamID 区分本机玩家与被观战的队友。死亡后，队友的血量变化和本机玩家恢复到 100 血都不会触发切回；检测到下一回合开始（进入冻结时间）、游戏由热身进入正式阶段，或者当前游戏结束时，才会自动切换回 CS2。每次切回会立即尝试一次，并以 400 毫秒为间隔再尝试两次，提高全屏窗口切换的成功率。

GSI 配置格式、可订阅的数据和示例载荷可查看 Valve Developer Community 的官方参考：[Counter-Strike: Global Offensive Game State Integration](https://developer.valvesoftware.com/wiki/Counter-Strike:_Global_Offensive_Game_State_Integration)。该 GSI 接口同样由 CS2 使用。

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
3. 如果 CS2 正在运行，请重启游戏，让 GSI 配置生效。旧版本升级后也需要重新安装一次监听配置。
4. 选择“切换到程序”并浏览目标 `.exe`，或选择“发送快捷键”后点击快捷键框录制组合键。
5. 按需勾选“暂停音乐播放器”和“暂停网页视频”。
6. 点击“开始监听”。状态显示“监听中”后即可进入游戏。

监听器只绑定 `127.0.0.1:3000`，不会接收局域网或互联网连接。配置保存在 `%LOCALAPPDATA%\CSMoyu\settings.ini`。

### 油猴脚本（可选）

[`tools/csmoyu-media-pause.user.js`](tools/csmoyu-media-pause.user.js) 可用于哔哩哔哩和抖音。把脚本导入 Tampermonkey 后，从对应网页切到 CS2、其他应用、其他标签页或最小化浏览器时，页面中的视频和音频会自动暂停。纯网页脚本无权读取 Windows 前台进程，因此无法只识别 `cs2.exe`；若要求严格限定为 CS2，需要由本工具向脚本提供本机状态接口。

![](/doc_img/CSMoyu.png)

## 注意

- 只有在接收到 `health > 0` 后再次收到 `health == 0` 才触发，因此不会因重复 GSI 数据连续切换。
- 监听采用 GSI 推送而不是主动轮询。玩家状态通道限制为 1 Hz；回合阶段使用独立的轻量通道，最高 10 Hz，但只在 `map/round` 状态变化时推送。程序内还会在存活期间跳过过密的普通玩家状态；死亡后则及时处理回合开始或游戏结束事件。
- 安装监听配置时会记录 `cs2.exe` 的路径；如果尚未记录，下一回合开始或游戏结束时会按进程名查找 CS2 窗口。
- 若目标程序以管理员身份运行，本工具也可能需要以管理员身份运行，Windows 才允许它激活窗口或发送按键。
- 暂停网页视频通过浏览器窗口标题识别当前活动标签页；浏览器若隐藏站点名称、视频位于后台标签页，或网站未响应 Windows 媒体命令，可能无法暂停。此功能无需安装浏览器扩展，也不会读取浏览记录。
- 端口 3000 被其他程序占用时，界面会显示监听失败。
