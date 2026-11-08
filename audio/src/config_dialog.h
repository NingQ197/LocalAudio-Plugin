#ifndef LA_CONFIG_DIALOG_H
#define LA_CONFIG_DIALOG_H

#include <string>

namespace la {

/* 在独立线程里弹出配置窗口，立即返回，不阻塞 TeamSpeak 客户端界面 */
void ShowConfigDialog();

/* 由插件初始化时调用，告诉窗口配置文件放在哪 */
void SetConfigPathForDialog(const std::wstring& path);

/* 插件被卸载时用来关掉窗口，避免窗口线程访问已卸载的代码 */
void CloseConfigDialog();
bool IsConfigDialogOpen();

} // namespace la

#endif /* LA_CONFIG_DIALOG_H */
