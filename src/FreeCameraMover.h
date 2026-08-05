#pragma once

namespace FreeCameraMover
{
	// 安装自由相机移动控制逻辑。
	// 应在 CommonLib/地址库初始化完成后调用（例如 SKSE 消息接口的 kPostLoad 或 kDataLoaded）。
	void Install();
}
