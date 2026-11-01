# Windows 窗口采集

Windows 窗口来源由媒体 worker 使用 Windows Graphics Capture（WGC）与 D3D11 采集；业务插件只使用公开的 `media.session / 1`，不接触原生采集对象或句柄。

## 行为

- 只采集选定的窗口。窗口最小化时暂停取帧，恢复后继续；窗口关闭或原生身份失效时返回明确错误，不静默改用屏幕区域或其他窗口。
- 无重绘的静态窗口在启动时即可取得真实首帧，不要求来源先重绘；首帧内容尺寸按捕获项尺寸确定，不用包含不可见边框的窗口矩形替代。
- 启动和采集中查询 `GetWindowDisplayAffinity`：仅当查询成功且返回 `WDA_MONITOR` 或 `WDA_EXCLUDEFROMCAPTURE` 时，以 `Window capture denied: source is protected from capture` 结束采集。查询失败、静态画面、最小化和暂时无帧都不判为故障。该检查目前只在未发布候选中生效；正式媒体包仍可能只冻结画面，见[当前遗留问题](limitations.md)。
- 该检查只覆盖明确的窗口保护标记，不能识别锁屏、安全桌面、UAC 或其他捕获权限拒绝；这些真实环境尚未验证。
- 要求系统支持 Windows Graphics Capture 和 D3D11；不支持或创建失败时返回具体错误，不静默改用桌面区域捕获。屏幕与摄像头来源仍使用 Qt，其他平台的窗口路径未更换。

正式包与未发布候选在受保护窗口上的差异、以及当前未覆盖的平台项见[当前遗留问题](limitations.md)。
