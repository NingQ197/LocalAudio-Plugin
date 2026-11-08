# 从源码构建

同一套工具可以生成音频版（`audio`）和屏幕版（`screen`）。插件功能内核版本为 2.5.0，安装器版本为 2.5.3。

## 工具和依赖

- Windows x64、Python 3.12+、CMake 3.20+、Ninja。
- 主插件使用 LLVM-MinGW x64 UCRT C++17 工具链。
- 原生界面桥使用 MSVC x64 和 **Qt 5.15.2 MSVC ABI**，与 TS 3.6.2 的 Qt 运行库匹配。
- 独立安装包使用 NSIS 3.10，需要用户已有官方 TeamSpeak 3.6.2（64 位）。

```powershell
python -m pip install -r requirements-build.txt
python tools/fetch_dependencies.py
```

下载脚本校验固定版本依赖的 SHA256，包括 RNNoise v0.2、模型数据、nlohmann/json 3.12.0、WebView2 SDK、Sherpa-ONNX 2.0.1 和 GTCRN 模型。`third_party/ts3sdk` 提供插件 API 26 声明；`third_party/sherpa/c-api.h` 提供动态加载声明。

## 1. 编译原生 Qt 界面桥

在 MSVC x64 开发命令行中，设置 Qt 开发目录：

```powershell
cmake -S native-ui -B build/native-ui -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<Qt-5.15.2-msvc2019_64>
cmake --build build/native-ui --config Release
Copy-Item build/native-ui/Release/ningq_denoise_ui.dll deps/runtime/
```

界面桥动态使用客户端已有的 Qt 库，安装器不携带 Qt DLL。

## 2. 生成资源清单

```powershell
python tools/prepare_resources.py audio --runtime deps/runtime --output build/bundle-audio --encrypt
```

屏幕版将 `audio` 换为 `screen`。省略 `--encrypt` 可生成可读界面资源。每次使用新的输出目录；加密构建会生成独立资源密钥，生成的头文件只供本次 DLL 编译使用，不应提交至仓库。

## 3. 编译插件

将 LLVM-MinGW 的 `clang` / `clang++` 加入 PATH 后执行：

```powershell
cmake -S . -B build/audio -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLA_EDITION=audio -DLA_RESOURCE_BUNDLE=<absolute-bundle-audio-path>
cmake --build build/audio
```

输出为 `build/audio/ts3localaudio.dll` 和 `build/audio/client-check.exe`。屏幕版使用 `-DLA_EDITION=screen`，并指定对应资源目录与构建目录。

## 4. 构建独立安装器

从微软官方渠道取得 x64 WebView2 与 VC++ 离线安装程序。脚本中的 SHA256 固定依赖版本；升级依赖时应同步更新哈希。

```powershell
python tools/build_installer.py --edition audio --bundle build/bundle-audio --dll build/audio/ts3localaudio.dll --helper build/audio/client-check.exe --webview-installer <official-WebView2-offline.exe> --vc-installer <official-vc_redist.x64.exe> --makensis <NSIS-3.10-makensis.exe> --output <LocalAudio-Audio.exe>
```

安装器面向 Windows 10 21H2（19044）及更新的 64 位系统。DLL 与资源目录必须成套部署；资源目录中的 `release-info.json` 记录本次构建文件的哈希。
