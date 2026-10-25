#include "FreeCameraMover.h"

#include "RE/Skyrim.h"
#include "RE/C/CommandTable.h"
#include "REL/Relocation.h"
#include "SKSE/Trampoline.h"

#include <windows.h>

#include <numbers>

#include "DKUtil/Logger.hpp"

namespace
{
	using namespace RE;

	// ============================================================================
	// 可调参数
	// ============================================================================

	// 自由相机模式下，自定义移动的基础速度（每帧位移量）。
	// 原 0.03F / 帧 ≈ 1.8 单位/秒（≈ 1.3 m/s），太慢；改为 0.10F 后约 4.2 m/s。
	inline constexpr float kMoveSpeed = 2.00F;
	// 鼠标灵敏度：每像素 → 弧度
	inline constexpr float kMouseSensitivity = 0.005F;
	// pitch 限位。
	//   设为 π（180°）相当于"基本不限制"：因为 pitch 不会累加到这么大。
	//   实际效果：玩家可以自由俯仰，但接近 ±90° 时水平 yaw 控制会因 cos→0 暂时失效，
	//   超过 ±90° 后摄像机会"翻转"（头朝下）。这是欧拉角累加的固有限制。
	inline constexpr float kMaxPitch = std::numbers::pi_v<float>;

	// 按键映射（Win32 虚拟键码 VK_*）
	//   WASD          水平移动（含 forward 跟随俯仰）
	//   Shift（按住）  鼠标控制视角（无限鼠标：每帧把光标拉回屏幕中心）
	inline constexpr std::uint32_t kKeyForward     = 0x57;  // W
	inline constexpr std::uint32_t kKeyBackward    = 0x53;  // S
	inline constexpr std::uint32_t kKeyLeft        = 0x41;  // A
	inline constexpr std::uint32_t kKeyRight       = 0x44;  // D
	inline constexpr std::uint32_t kKeyMouseRotate = 0x10;  // VK_SHIFT

	// ============================================================================
	// TESCameraState vtable：
	//   0: ~TESCameraState   1: Begin   2: End   3: Update
	// ============================================================================

	// 原始函数指针
	using FreeUpdate_t     = void(FreeCameraState*, BSTSmartPointer<TESCameraState>&);
	using BleedoutUpdate_t = void(BleedoutCameraState*, BSTSmartPointer<TESCameraState>&);
	using SetState_t       = void(TESCamera*, TESCameraState*);

	REL::Relocation<FreeUpdate_t>     g_originalFreeUpdate;
	REL::Relocation<BleedoutUpdate_t> g_originalBleedoutUpdate;
	// SetState 是非虚成员函数；用 REL::Relocation 统一管理跳板地址。
	// write_call<5> 返回 std::uintptr_t，可通过 operator=(std::uintptr_t) 赋值。
	REL::Relocation<SetState_t>       g_originalSetState;

	// 鼠标状态：右键拖动时记录上一帧光标位置
	POINT g_lastMousePos{};
	bool  g_hasLastMousePos = false;

	// ----------------------------------------------------------------------------
	// 死亡 + tfc 劫持状态
	// ----------------------------------------------------------------------------
	// 玩家已死 + 收到 tfc 控制台命令时设为 true，
	// 直到玩家复活或再次输入 tfc 关闭才清零。
	// 用来驱动 BleedoutUpdate / SetState 的劫持逻辑。
	// 注意：Skyrim 原版 tfc 在死亡时会被内部拒绝，并不会真正把相机切到 Free，
	//       也不能用 IsInFreeCameraMode()（死亡下 currentState 永远是 Bleedout）。
	bool g_deathTfcActive = false;

	// tfc 控制台命令的原始 executeFunction
	// 类型见 RE::SCRIPT_FUNCTION::Execute_t
	SCRIPT_FUNCTION::Execute_t* g_originalTfcExecute = nullptr;

	// ----------------------------------------------------------------------------
	// 工具
	// ----------------------------------------------------------------------------
	[[nodiscard]] bool IsKeyPressed(std::uint32_t a_vkCode)
	{
		return (GetAsyncKeyState(static_cast<int>(a_vkCode)) & 0x8000) != 0;
	}

	// 死亡 + tfc 劫持已激活：是否启用「死亡自由相机」覆写
	[[nodiscard]] bool ShouldOverrideDeathCamera()
	{
		auto* cam = PlayerCamera::GetSingleton();
		auto* player = PlayerCharacter::GetSingleton();
		if (!cam || !player) {
			return false;
		}
		return g_deathTfcActive && player->IsDead(false);
	}

	// ----------------------------------------------------------------------------
	// WASD 位移
	// ----------------------------------------------------------------------------
	// W / S 沿摄像机视线方向（含俯仰），A / D 沿水平面内左右方向。
	// 注意 forward 必须带上 pitch，否则 W 在俯视/仰视时只向水平方向移动，
	// 与"W 向镜头正前方"不符。
	void UpdatePosition(FreeCameraState* a_state)
	{
		if (!a_state) {
			return;
		}

		const float yaw   = a_state->rotation.y;
		const float pitch = a_state->rotation.x;
		const float cosP  = std::cos(pitch);
		const float sinP  = std::sin(pitch);

		// 摄像机视线正方向（含俯仰）：
		//   pitch = 0   → (sin yaw, cos yaw, 0)     水平
		//   pitch = +π/2 → (0, 0, -1)                俯视看地
		//   pitch = -π/2 → (0, 0, +1)                仰视看天
		const NiPoint3 forward{ std::sin(yaw) * cosP, std::cos(yaw) * cosP, -sinP };
		// 水平面内的右向量（与 yaw 正交）
		const NiPoint3 right{ std::cos(yaw), -std::sin(yaw), 0.0F };

		NiPoint3 direction{ 0.0F, 0.0F, 0.0F };
		if (IsKeyPressed(kKeyForward))  direction += forward;
		if (IsKeyPressed(kKeyBackward)) direction -= forward;
		if (IsKeyPressed(kKeyRight))    direction += right;
		if (IsKeyPressed(kKeyLeft))     direction -= right;

		const float length = direction.Length();
		if (length <= std::numeric_limits<float>::epsilon()) {
			return;
		}
		direction /= length;

		a_state->translation += direction * kMoveSpeed;
	}

	// ----------------------------------------------------------------------------
	// 鼠标视角（按住 Shift 时）
	// ----------------------------------------------------------------------------
	// 原版 FreeCameraState::ProcessMouseMove 走 PlayerControls 事件流，
	// 死亡 UI（黑色遮罩、Bleedout 菜单）会直接吞掉这些事件，导致视角无法旋转。
	//
	// 交互模式：
	//   1) 按下 Shift 的瞬间，把光标瞬移到屏幕中心；
	//   2) 此后**不再 SetCursorPos**（否则玩家物理移动鼠标后会被 Windows 瞬移回去，
	//      感到"鼠标被强制拉回"）；
	//   3) 玩家自然移动鼠标，累加 dx/dy 到 rotation；
	//   4) 鼠标移到屏幕边缘后若还想继续旋转，再次按 Shift 重新从中心开始；
	//   5) 释放 Shift：停止控制，鼠标位置保持不变。
	void UpdateMouseRotation(FreeCameraState* a_state)
	{
		if (!a_state) {
			return;
		}

		if (!IsKeyPressed(kKeyMouseRotate)) {
			// Shift 未按住：清空历史位置，让下次按下重新从中心开始
			g_hasLastMousePos = false;
			return;
		}

		if (!g_hasLastMousePos) {
			// 第一次按下 Shift：把光标瞬移到屏幕中心并记录为起点
			const int sw = GetSystemMetrics(SM_CXSCREEN);
			const int sh = GetSystemMetrics(SM_CYSCREEN);
			g_lastMousePos.x = sw / 2;
			g_lastMousePos.y = sh / 2;
			SetCursorPos(g_lastMousePos.x, g_lastMousePos.y);
			g_hasLastMousePos = true;
			// 第一帧不旋转（避免以玩家原本的鼠标位置为起点产生跳变）
			return;
		}

		POINT cur{};
		if (!GetCursorPos(&cur)) {
			return;
		}

		// dx/dy = 玩家本帧物理移动鼠标的距离（起点是上一次的鼠标位置）
		const float dx = static_cast<float>(cur.x - g_lastMousePos.x) * kMouseSensitivity;
		const float dy = static_cast<float>(cur.y - g_lastMousePos.y) * kMouseSensitivity;

		// yaw：右移鼠标（dx>0）→ 视角向右转。
		//   forward = (sin yaw, cos yaw, 0)，yaw=0 朝 +Y，yaw=π/2 朝 +X。
		//   因此"向右看"应让 yaw 增大，即 +dx。
		a_state->rotation.y += dx;
		// pitch：下移鼠标（dy>0）→ 视角向下俯视。
		//   本插件约定 pitch 增大 = 俯视（看 -Z），因此 +dy。
		//   不再 clamp：玩家可以自由俯仰超过 ±90°，但接近/超过 ±90° 时
		//   由于 cos(pitch) 衰减和欧拉角翻转，画面会出现"翻转"——这是 BSTPoint2
		//   表示 rotation 的固有限制，介意的话改用四元数累加。
		a_state->rotation.x += dy;

		// 记录当前鼠标位置作为下一帧的起点
		g_lastMousePos = cur;
	}

	// ============================================================================
	// 钩子实现
	// ============================================================================

	// ---- FreeCameraState::Update ------------------------------------------------
	// 在原版 Update 之后叠加自定义 WASD / 鼠标。
	void HookFreeCameraUpdate(FreeCameraState* a_this, BSTSmartPointer<TESCameraState>& a_nextState)
	{
		auto* cam    = PlayerCamera::GetSingleton();
		auto* player = PlayerCharacter::GetSingleton();

		// 先执行原版逻辑（旋转标志、退出检测、tfc 时序相关字段等）。
		g_originalFreeUpdate(a_this, a_nextState);

		// 死亡劫持中：原版 FreeCameraState::Update 在玩家死亡时会主动把
		// a_nextState 切到 BleedoutCameraState 并直接 return，根本不处理
		// 位置/旋转。此时我们必须：
		//   1) 强制把 a_nextState 改回 Free（避免下一帧跳到 Bleedout），
		//   2) 总是施加自定义移动和鼠标视角（因为原版不做事）。
		if (cam && player && player->IsDead(false) && g_deathTfcActive) {
			a_nextState = cam->GetRuntimeData().cameraStates[CameraState::kFree];
			UpdatePosition(a_this);
			UpdateMouseRotation(a_this);
			return;
		}

		// 正常情况：原版 Update 通常把 a_nextState 保持为 this（即 Free），
		// 但 tfc 关闭时会切到其它状态。只有仍处于 Free 时才施加自定义移动 / 鼠标。
		if (a_nextState && a_nextState->id == CameraState::kFree) {
			UpdatePosition(a_this);
			UpdateMouseRotation(a_this);
		}
	}

	// ---- BleedoutCameraState::Update --------------------------------------------
	// 死亡状态下，PlayerCamera::Update 每帧都会调 BleedoutCameraState::Update，
	// 并由它自己把 a_nextState 设回 BleedoutCameraState 自身，从而维持死亡相机。
	// 我们在「死亡 + tfc 劫持已激活」的情况下，把 a_nextState 强制改为 FreeCameraState，
	// 触发 PlayerCamera::SetState 的状态切换，从根上把死亡状态踢掉。
	void HookBleedoutUpdate(BleedoutCameraState* a_this, BSTSmartPointer<TESCameraState>& a_nextState)
	{
		if (ShouldOverrideDeathCamera()) {
			auto* cam = PlayerCamera::GetSingleton();
			a_nextState = cam->GetRuntimeData().cameraStates[CameraState::kFree];
			return;
		}
		g_originalBleedoutUpdate(a_this, a_nextState);
	}

	// ---- TESCamera::SetState 兜底 ----------------------------------------------
	// 仅做两件事：
	//   1) 死亡劫持中拦截所有切到 Bleedout 的请求（强制回 Free）
	//   2) 玩家复活时清零劫持标志
	void HookSetState(TESCamera* a_camera, TESCameraState* a_state)
	{
		auto* cam    = PlayerCamera::GetSingleton();
		auto* player = PlayerCharacter::GetSingleton();
		if (a_state && cam && player) {
			const auto* bleedoutState = cam->GetRuntimeData().cameraStates[CameraState::kBleedout].get();

			if (player->IsDead(false)) {
				if (a_state == bleedoutState && g_deathTfcActive) {
					// 死亡劫持中：拒绝切到 Bleedout，改切到 Free
					g_originalSetState(a_camera, const_cast<TESCameraState*>(cam->GetRuntimeData().cameraStates[CameraState::kFree].get()));
					return;
				}
			} else {
				// 玩家复活：清零劫持标志
				g_deathTfcActive = false;
			}
		}
		g_originalSetState(a_camera, a_state);
	}

	// ---- tfc 控制台命令钩子 -----------------------------------------------------
	// Skyrim 原版 tfc（ToggleFlyCam）在玩家死亡时会被内部拒绝：玩家死亡时
	// 相机 currentState 永远停在 BleedoutCameraState，无法被 IsInFreeCameraMode()
	// 识别为 true，因此原插件依赖的劫持条件永远不成立。
	// 我们接管 tfc 的 executeFunction：
	//   - 玩家已死 → 绕过原版逻辑，强制把相机切到 FreeCameraState 并启动劫持，
	//                之后 BleedoutUpdate / SetState 的钩子会维持 Free 状态。
	//   - 玩家未死 → 调用原版 tfc，让它按 Skyrim 原生行为切换自由相机。
	bool HookTfcExecute(
		const SCRIPT_PARAMETER*                              a_paramInfo,
		SCRIPT_FUNCTION::ScriptData*                         a_scriptData,
		TESObjectREFR*                                       a_thisObj,
		TESObjectREFR*                                       a_containingObj,
		Script*                                              a_scriptObj,
		ScriptLocals*                                        a_locals,
		double&                                              a_result,
		std::uint32_t&                                       a_opcodeOffsetPtr)
	{
		auto* cam    = PlayerCamera::GetSingleton();
		auto* player = PlayerCharacter::GetSingleton();

		if (cam && player && player->IsDead(false)) {
			// 死亡时：toggle 死亡自由相机劫持。
			//   劫持未激活 → 强制切到 Free 模式，激活劫持。
			//   劫持已激活 → 切回 Bleedout 相机，关闭劫持。
			if (g_deathTfcActive) {
				g_deathTfcActive = false;
				cam->SetState(cam->GetRuntimeData().cameraStates[CameraState::kBleedout].get());
			} else {
				g_deathTfcActive = true;
				// 把 FreeCameraState 重置到玩家当前位置。
				//   FreeCameraState 是 PlayerCamera::cameraStates[kFree] 持有的全局单例，
				//   每次 tfc 切换都复用同一个对象；translation / rotation 会保留上次
				//   tfc 留下的值，所以这里在切到 Free 之前显式重置。
				if (auto* freeState = static_cast<FreeCameraState*>(cam->GetRuntimeData().cameraStates[CameraState::kFree].get())) {
					freeState->translation = player->GetPosition();
					freeState->rotation.y  = player->GetAngleZ();
					freeState->rotation.x  = 0.0F;  // 水平视角起步
				}
				cam->SetState(cam->GetRuntimeData().cameraStates[CameraState::kFree].get());
			}
			a_result = 1.0;
			return true;
		}

		// 玩家未死：转回原版 tfc（开关自由视角 / 冻结时间）。
		//   兜底清零死亡劫持标志（覆盖"玩家从死亡中复活但劫持标志未清"的情况）。
		g_deathTfcActive = false;
		if (g_originalTfcExecute) {
			return g_originalTfcExecute(a_paramInfo, a_scriptData, a_thisObj, a_containingObj, a_scriptObj, a_locals, a_result, a_opcodeOffsetPtr);
		}
		return false;
	}

	// ============================================================================
	// 安装
	// ============================================================================

	void InstallVFuncHooks()
	{
		// FreeCameraState 主 vtable（继承自 TESCameraState 的部分）
		REL::Relocation<std::uintptr_t> freeVTable{ RE::VTABLE_FreeCameraState[0] };
		g_originalFreeUpdate = freeVTable.write_vfunc(3, &HookFreeCameraUpdate);

		// BleedoutCameraState 主 vtable
		REL::Relocation<std::uintptr_t> bleedVTable{ RE::VTABLE_BleedoutCameraState[0] };
		g_originalBleedoutUpdate = bleedVTable.write_vfunc(3, &HookBleedoutUpdate);
	}

	void InstallSetStateHook()
	{
		// TESCamera::SetState 是非虚成员函数，CommonLib 提供了包装
		// （见 Library/CommonLibSSE-NG/src/RE/T/TESCamera.cpp）。
		// 通过 Offset::TESCamera::SetState 取到该函数真实入口，再用 SKSE trampoline
		// 在入口处插入一个 5 字节的 CALL 跳转到 HookSetState。
		// write_call 返回的是 trampoline 跳板（5 字节之后的继续执行入口），
		// 我们把它保存为「原函数」，以便 HookSetState 末尾调用以继续原逻辑。
		REL::Relocation<SetState_t> setState{ REL::RelocationID(32290, 33026) };  // TESCamera::SetState

		auto& trampoline = SKSE::GetTrampoline();
		g_originalSetState = trampoline.write_call<5>(
			reinterpret_cast<std::uintptr_t>(setState.get()),
			&HookSetState);
	}

	// 通过遍历控制台命令表（按 shortName == "tfc" 匹配）找到 tfc 命令的 SCRIPT_FUNCTION 项。
	// 这样不依赖 longName 字段的拼写（不同版本/补丁可能登记为 "ToggleFlyCam" 或 "tfc"），
	// 也能在 SE / AE 通用。
	SCRIPT_FUNCTION* FindTfcCommand()
	{
		auto* firstCmd = SCRIPT_FUNCTION::GetFirstConsoleCommand();
		if (!firstCmd) {
			return nullptr;
		}
		for (std::uint16_t i = 0; i < SCRIPT_FUNCTION::Commands::kConsoleCommandsEnd; ++i) {
			const auto* sn = firstCmd[i].shortName;
			if (sn && std::strlen(sn) == 3 && _strnicmp(sn, "tfc", 3) == 0) {
				return &firstCmd[i];
			}
			// 兜底：部分表项 shortName 为空，但 functionName 直接写作 "tfc"
			const auto* fn = firstCmd[i].functionName;
			if (fn && std::strlen(fn) == 3 && _strnicmp(fn, "tfc", 3) == 0) {
				return &firstCmd[i];
			}
		}
		return nullptr;
	}

	void InstallConsoleCommandHook()
	{
		auto* tfcCmd = FindTfcCommand();
		if (!tfcCmd) {
			// 最后兜底：直接走 LocateConsoleCommand（长名 "ToggleFlyCam" 是 tfc 的语义名）
			tfcCmd = SCRIPT_FUNCTION::LocateConsoleCommand("ToggleFlyCam"sv);
		}
		if (!tfcCmd) {
			ERROR("tfc console command not found in SCRIPT_FUNCTION table");
			return;
		}

		g_originalTfcExecute = tfcCmd->executeFunction;
		tfcCmd->executeFunction = &HookTfcExecute;
		INFO("tfc console command hooked (original = {:p})", static_cast<void*>(g_originalTfcExecute));
	}
}

namespace FreeCameraMover
{
	void Install()
	{
		static bool installed = false;
		if (installed) {
			return;
		}

		// 留出 trampoline 空间：vfunc 钩子（write_vfunc）需要少量；
		// SetState 钩子（write_call）需要 14 字节。
		SKSE::AllocTrampoline(1 << 6);

		// 必须在 CommonLib/地址库完全就绪后才能 LocateConsoleCommand，
		// 故放在 SKSE::RegisterForAPIInitEvent 内调用 Install()。
		InstallConsoleCommandHook();
		InstallVFuncHooks();
		InstallSetStateHook();

		installed = true;

		INFO("FreeCameraMover installed (tfc override + Bleedout camera override + SetState guard)");
	}
}
