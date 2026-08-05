#include "FreeCameraMover.h"
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>  // CLS内置的日志库

DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	DKUtil::Logger::Init(Plugin::NAME, REL::Module::get().version().string());

	//REL::Module::reset();
	SKSE::Init(a_skse);

	INFO("{} v{} loaded", Plugin::NAME, Plugin::Version);

	// 安装死亡自由视角相机的自由移动控制逻辑。
	// 使用 API 初始化事件确保 CommonLib/地址库完全就绪后再 hook，兼容 SE / AE。
	SKSE::RegisterForAPIInitEvent(
		[]()
		{
			FreeCameraMover::Install();
		});

	return true;
}
