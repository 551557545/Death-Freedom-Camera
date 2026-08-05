# Death Freedom Camera

**[English](README.md)** | **简体中文**

一个用于 **Skyrim Special Edition 1.5.x** 和 **Skyrim Anniversary Edition 1.6.x** 的 SKSE 插件。功能：在玩家死亡状态下，通过拦截控制台命令 `tfc`（Toggle Free Camera）实现自由视角移动。

构建时使用 **FLATRIM preset**，输出一个**同时支持 SE 和 AE** 的单一二进制（运行时按 `REL::Module::IsAE()` 自动选择偏移表）。

---

## 功能说明

Skyrim 原版在玩家死亡时会**静默拒绝** `tfc` 命令，相机永远停在死亡状态。本插件 hook 了 `tfc` 控制台命令和 `TESCamera::SetState`，实现：

- 死亡状态下输入 `tfc` → 强制把相机切到自由相机模式并保持
- 再次输入 `tfc` → 恢复死亡相机
- 玩家复活 → 自动清除劫持

由于死亡 UI 会吞掉 `PlayerInputHandler` / `ProcessMouseMove` 的事件流，**自定义的 WASD 位移和鼠标视角**直接基于 Win32 `GetAsyncKeyState` / `GetCursorPos` 叠加在 `FreeCameraState` 上，不依赖原版事件流。

---

## 安装

1. 确认已安装 **SKSE64**（对应你的 Skyrim 版本）。
2. 把 `Death_Freedom_Camera.dll`（如果需要符号文件可一并放 `.pdb`）放到：
   ```
   <Skyrim 目录>/SKSE/Plugins/
   ```
3. 启动游戏后确认插件已加载，查看 `SKSE/plugins.log` 应能看到类似：
   ```
   Death_Freedom_Camera v1.0.0.0 loaded
   tfc console command hooked
   FreeCameraMover installed (tfc override + Bleedout camera override + SetState guard)
   ```

不需要 INI、MCM、ESP 等额外配置文件。

---

## 使用方式

### 1. 死亡后开启自由相机

1. 玩家进入死亡状态（bleedout）。
2. 按 `~` 打开控制台。
3. 输入：
   ```
   tfc
   ```
4. 相机切到自由相机模式，关闭控制台后即可看到世界。

### 2. 关闭

在控制台再次输入 `tfc`，相机回到死亡相机。**直接复活**也会自动清除劫持。

> 注意：死亡劫持激活期间，本插件**不会**调用原版 `tfc` 的处理函数，所以原版 `tfc` 自带的"冻结时间"副作用不会生效。想冻结时间请先复活退出劫持再开 tfc。再次 `tfc` 或复活即可解除劫持。

### 3. 按键

| 按键 | 行为 |
|---|---|
| `W` / `S` | 沿**摄像机正前方**前后移动（**带俯仰**：俯视时向下飞、仰视时向上飞） |
| `A` / `D` | 水平左右平移（不含俯仰） |
| `Shift`（按住） | 鼠标控制视角，详见下节 |
| 鼠标 | 按住 `Shift` 时移动鼠标旋转视角 |
| `tfc`（控制台） | 切换死亡自由相机劫持的开关 |

### 4. 鼠标视角

- **按住 `Shift`** 进入鼠标视角模式。
- **按下的瞬间**：鼠标光标瞬移到屏幕中心；视角**不**瞬移（避免画面跳变）。
- **持续按住 `Shift` 期间**：鼠标光标**不会**每帧被拉回，你可以自由移动鼠标，`dx/dy` 累加到视角旋转。
- 鼠标移到屏幕边缘想继续旋转：**松开再按一次** `Shift`，重新从中心开始。
- **松开 `Shift`**：停止鼠标视角，鼠标位置保持不变。

> 实现说明：插件用 `GetCursorPos`（Windows 屏幕坐标）而不是 DirectInput 的 raw mouse delta，所以经典的"每帧把鼠标拉回屏幕中心"无限鼠标模式会让玩家感觉"鼠标被强制拉回"。这里采用"按下时瞬移一次 + 不再拉回"的方案，鼠标行为更自然。

### 5. 俯仰范围

pitch **没有限位**，可以自由抬头/低头。

由于 `FreeCameraState::rotation` 用的是欧拉角（`BSTPoint2<float>`：pitch + yaw）而不是四元数，有两个固有局限：

- **接近 ±90° 时**：水平 yaw 的视觉影响被 `cos(pitch) → 0` 大幅削弱，水平鼠标视角会感觉"卡住"。
- **超过 ±90° 后**：摄像机会"翻转"（头朝下），再继续移动鼠标相当于绕摄像机自身轴继续转。

这是欧拉角累加的固有限制，不是 bug。如果想要完全无翻转的版本，需要用四元数累加（外部维护 `NiQuaternion` 状态，读欧拉角 → 转四元数 → 应用 delta → 转回欧拉角），工作量稍大。如有需要可联系作者升级。

---

## 兼容性

- **引擎**：Skyrim Special Edition 1.5.97+ 和 Skyrim Anniversary Edition 1.6.x（FLATRIM 单二进制，运行时自动选择）
- **框架**：CommonLibSSE-NG
- **SKSE**：2.2.6+
- **其他 mod**：不触碰任务、ESP/ESL、Papyrus 数据，所以任务/玩法 mod 不受影响。
  - **不**与同样 hook `TESCameraState::Update` vtable 0x3（`FreeCameraState` / `BleedoutCameraState`）或劫持 `tfc` 控制台命令的 mod 兼容
  - 不影响 ENB、UI、Skyrim Settings 等

---

## 从源码构建

### 依赖

- **Visual Studio 2022**（含"使用 C++ 的桌面开发"工作负载）
- **CMake ≥ 3.21**
- **PowerShell**（`_build.bat` 内部调用 vcvars64）
- **vcpkg** + `xbyak`（由根目录 `CMakePresets.json` 处理）
- 一份可用的 `CommonLibSSE-NG`（参考根目录 `README.md` 和 `CMakePresets.json` 的环境变量配置：`CommonLibSSEPath`、`DKUtilPath`、`VCPKG_ROOT`）

### 配置与构建

在仓库根目录执行：

```bat
:: 配置（FLATRIM = SE + AE 单一二进制）
cmake -B Build -S . --preset=FLATRIM -Wno-dev

:: 构建本插件
msbuild "Build/Plugins/Death_Freedom_Camera/Death_Freedom_Camera.vcxproj" ^
        /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

或直接跑根目录的脚本：

```bat
_build.bat
```

产物路径：`Build/bin/Release/Death_Freedom_Camera.dll`。

---

## 钩子清单（给插件作者 / 代码审查者）

| 钩子 | 目标 | 作用 |
|---|---|---|
| `tfc` 控制台命令（`SCRIPT_FUNCTION::executeFunction`） | `tfc` | toggle 死亡劫持；死亡时绕过原版 tfc |
| `FreeCameraState::Update` vfunc 3 | `cameraStates[kFree]` | 施加 WASD 位移和 Shift+鼠标视角；死亡劫持时强制 `a_nextState = Free` |
| `BleedoutCameraState::Update` vfunc 3 | `cameraStates[kBleedout]` | 死亡劫持时强制 `a_nextState = Free`（兜底） |
| `TESCamera::SetState`（trampoline，5 字节 CALL） | `TESCamera` | 死亡劫持中拒绝一切切到 `BleedoutCameraState` 的请求；玩家复活时清零劫持标志 |

全局标志 `g_deathTfcActive` 是劫持状态的**唯一来源**，仅由 `HookTfcExecute` 和 `HookSetState` 的复活分支读写。

---

## 许可

见仓库根目录 `LICENSE`。
