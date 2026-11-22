#include "i18n.h"
#include "common.h"
#include "json.h"
#include <algorithm>
#include <map>
#include <windows.h>

namespace {

std::wstring g_code = L"zh-CN";
bool g_loaded = false;
std::vector<UiLanguage> g_languages;
std::map<std::wstring, std::map<std::wstring, std::wstring>> g_external;
std::vector<std::pair<std::wstring, std::wstring>> g_pairs;

const std::map<std::wstring, std::wstring>& EnglishMap() {
    static const std::map<std::wstring, std::wstring> map = {
        { L"打开", L"Open" }, { L"刷新", L"Refresh" }, { L"保存", L"Save" },
        { L"导出", L"Export" }, { L"设置", L"Settings" }, { L"浅色", L"Light" },
        { L"深色", L"Dark" }, { L"源码", L"Source" }, { L"预览", L"Preview" },
        { L"大纲", L"Outline" }, { L"画笔", L"Draw" }, { L"折叠", L"Collapse" },
        { L"展开", L"Expand" }, { L"主页", L"Home" }, { L"画布", L"Canvas" },
        { L"关闭", L"Close" }, { L"取消", L"Cancel" }, { L"复制", L"Copy" },
        { L"粘贴", L"Paste" }, { L"组合", L"Group" }, { L"拆组", L"Ungroup" },
        { L"旋转", L"Rotate" }, { L"删除", L"Delete" }, { L"选", L"Select" },
        { L"荧", L"Marker" }, { L"笔", L"Pen" }, { L"擦", L"Eraser" },
        { L"线", L"Line" }, { L"箭", L"Arrow" }, { L"框", L"Rect" },
        { L"圆", L"Ellipse" }, { L"字", L"Text" }, { L"调", L"Colors" },

        { L"外观", L"Appearance" }, { L"滚轮速度", L"Wheel speed" },
        { L"背景图片", L"Background image" }, { L"选择图片", L"Choose image" },
        { L"许可与贡献者", L"License & contributors" },
        { L"选择背景图片", L"Choose background image" },
        { L"未设置背景图片", L"No background image" },
        { L"（文件不存在）", L" (file missing)" },
        { L"背景不透明度", L"Background opacity" },
        { L"5% – 60%，默认 15%", L"5% – 60%, default 15%" },
        { L"5% – 60%，默认 15%；位置仅在铺满裁剪时生效",
          L"5% – 60%, default 15%; position applies to Cover only" },
        { L"填充方式", L"Fit" }, { L"铺满裁剪", L"Cover" },
        { L"完整显示", L"Contain" }, { L"拉伸", L"Stretch" }, { L"平铺", L"Tile" },
        { L"裁剪位置", L"Crop position" }, { L"左上", L"TL" },
        { L"右上", L"TR" }, { L"左下", L"BL" }, { L"右下", L"BR" },
        { L"上", L"T" }, { L"下", L"B" }, { L"左", L"L" }, { L"右", L"R" },
        { L"中", L"C" },
        { L"5% – 60%，默认 15%；位置仅在铺满裁剪时生效",
          L"5% – 60%, default 15%; position applies to Cover only" },
        { L"图片文件不能超过 64 MB。", L"Image file must not exceed 64 MB." },
        { L"无法读取这个图片格式，请选择 PNG、JPEG、BMP、GIF、TIFF、WEBP 或 ICO 文件。",
          L"Cannot read this image format. Choose PNG, JPEG, BMP, GIF, TIFF, WEBP or ICO." },
        { L"图片尺寸过大，请选择不超过 6000 万像素的图片。",
          L"Image is too large. Choose an image with at most 60 megapixels." },
        { L"背景图片已更新。", L"Background image updated." },
        { L"背景图片已复制到配置目录。", L"Background image copied to the configuration folder." },
        { L"已清除背景图片。", L"Background image cleared." },
        { L"选择后复制到配置目录并生成 ID，原图移动或删除不影响背景",
          L"Copied to the configuration folder with an ID; moving the original does not break the background." },
        { L"背景图片文件不存在。", L"Background image file does not exist." },
        { L"无法创建背景图片目录。", L"Cannot create the background image folder." },
        { L"无法复制背景图片到配置目录。请检查配置目录写入权限。",
          L"Cannot copy the background image to the configuration folder. Check write permission." },
        { L"无法保存背景图片副本。", L"Cannot save the background image copy." },
        { L"无法生成唯一的背景图片 ID。", L"Cannot generate a unique background image ID." },
        { L"清除", L"Clear" },
        { L"文件关联与默认应用", L"File associations and defaults" },
        { L"全选", L"Select all" }, { L"加入打开方式", L"Add to Open With" },
        { L"取消注册", L"Unregister" }, { L"申请默认应用", L"Set default" },
        { L"Windows 默认应用", L"Windows defaults" },
        { L"取消选中格式的默认应用", L"Clear selected defaults" },
        { L"画笔工具按钮", L"Pen tools" }, { L"配置文件", L"Configuration" },
        { L"复制路径", L"Copy path" }, { L"定位", L"Show in folder" },
        { L"复制配置", L"Copy config" }, { L"快捷键", L"Shortcuts" },
        { L"恢复默认", L"Restore defaults" }, { L"清空本地数据", L"Clear local data" },
        { L"新建画布", L"New canvas" }, { L"白色背景", L"White background" },
        { L"黑色背景", L"Black background" }, { L"文本与编码", L"Text and encoding" },
        { L"自动检测", L"Auto detect" }, { L"重新检测", L"Detect again" },
        { L"修复乱码", L"Repair mojibake" }, { L"文档语言", L"Document language" },
        { L"自动", L"Auto" }, { L"中文", L"Chinese" }, { L"界面语言", L"Interface language" },
        { L"当前编码：", L"Encoding: " }, { L"当前没有活动文档", L"No active document" },
        { L"当前文档未加载完成", L"Document is not loaded" },

        { L"轻量本地文档阅读器", L"Lightweight local document reader" },
        { L"打开文件", L"Open file" }, { L"最近打开", L"Recent" },
        { L"纯文本", L"Plain text" }, { L"源代码", L"Source code" },
        { L"个节点", L" nodes" },
        { L"个工作表", L" sheets" }, { L"个段落", L" paragraphs" },
        { L"个块", L" blocks" }, { L"行", L" lines" }, { L"页", L" pages" },

        { L"导出位图", L"Export bitmap" }, { L"输出分辨率", L"Output resolution" },
        { L"宽度 (px)", L"Width (px)" }, { L"实际输出：", L"Output: " },
        { L"快捷比例", L"Presets" }, { L"导出 PNG", L"Export PNG" },
        { L"正在生成预览…", L"Generating preview…" }, { L"已导出：", L"Exported: " },

        { L"自定义颜色", L"Custom color" }, { L"自选颜色槽", L"Custom swatches" },
        { L"自选 1", L"Custom 1" }, { L"自选 2", L"Custom 2" },
        { L"十六进制", L"Hex" }, { L"使用并保存", L"Apply and save" },
        { L"支持 #RRGGBB / #AARRGGBB", L"Supports #RRGGBB / #AARRGGBB" },

        { L"已修复乱码", L"Encoding repaired" }, { L"编码待确认", L"Encoding uncertain" },
        { L"笔记未保存", L"Notes unsaved" }, { L"笔记文件异常", L"Notes file error" },
        { L"打开失败", L"Open failed" }, { L"就绪", L"Ready" },
        { L"拖入文件或按 Ctrl+O 打开", L"Drop a file or press Ctrl+O" },

        { L"外观", L"Appearance" }, { L"滚轮速度", L"Wheel speed" },
        { L"每格滚动 ", L"Wheel: " }, { L"文件关联与默认应用", L"File associations" },
        { L"默认", L"Default" }, { L"加入打开方式", L"Add to Open With" },
        { L"取消注册", L"Unregister" }, { L"申请默认应用", L"Set default" },
        { L"Windows 默认应用", L"Windows defaults" },
        { L"取消选中格式的默认应用", L"Clear selected defaults" },
        { L"画笔工具按钮", L"Pen tools" }, { L"配置文件", L"Configuration" },
        { L"复制路径", L"Copy path" }, { L"定位", L"Show in folder" },
        { L"复制配置", L"Copy config" }, { L"快捷键", L"Shortcuts" },
        { L"恢复默认", L"Restore defaults" }, { L"清空本地数据", L"Clear local data" },
        { L"新建画布", L"New canvas" }, { L"默认 ", L"Default " },
        { L"白色背景", L"White background" }, { L"黑色背景", L"Black background" },
        { L"文本与编码", L"Text and encoding" }, { L"自动检测", L"Auto detect" },
        { L"重新检测", L"Detect again" }, { L"修复乱码", L"Repair mojibake" },
        { L"文档语言", L"Document language" }, { L"界面语言", L"Interface language" },
        { L"简体中文", L"Simplified Chinese" }, { L"繁体中文", L"Traditional Chinese" },
        { L"日语", L"Japanese" }, { L"韩语", L"Korean" }, { L"西文", L"Western" },
        { L"简中", L"ZH-CN" }, { L"繁中", L"ZH-TW" },

        { L"最近打开", L"Recent" }, { L"打开文件", L"Open file" }, { L"纯文本", L"Plain text" },
        { L"打开文件夹", L"Open folder" }, { L"工作区", L"Workspace" },
        { L"未打开文件夹", L"No folder" }, { L"文件", L"Files" },
        { L"显示工作区", L"Show workspace" }, { L"隐藏工作区", L"Hide workspace" },
        { L"刷新工作区", L"Refresh workspace" }, { L"定位当前文件", L"Locate current file" },
        { L" · 加载中", L" · loading" },
        { L" · 已暂停", L" · paused" }, { L"最近工作区", L"Recent workspaces" },
        { L"清空全部最近工作区？", L"Clear all recent workspaces?" },
        { L"该工作区不存在或无法访问。", L"This workspace is unavailable." },
        { L"输入路径", L"Path" }, { L"输入路径 - LitheMD", L"Path - LitheMD" },
        { L"打开文件夹 - LitheMD", L"Open folder - LitheMD" },
        { L"阅读版式", L"Reading layout" },
        { L"字号", L"Font size" }, { L"正文宽度", L"Content width" },
        { L"窄", L"Narrow" }, { L"适中", L"Medium" }, { L"宽", L"Wide" },
        { L"超宽", L"Extra wide" }, { L"分屏", L"Split" },
        { L"演示模式", L"Presentation" }, { L"对比", L"Diff" },
        { L"文件已在外部修改", L"File changed externally" },
        { L"预览", L"Preview" }, { L"解压选中", L"Extract selected" },
        { L"全部解压", L"Extract all" }, { L"名称", L"Name" },
        { L"大小", L"Size" }, { L"修改时间", L"Modified" },
        { L"压缩包密码", L"Archive password" }, { L"选择解压位置", L"Extract to" },
        { L"密码", L"Password" }, { L"确定", L"OK" },
        { L"个文件", L" files" }, { L" 个文件", L" files" },
        { L"个文件夹", L" folders" }, { L" 个文件夹", L" folders" },
        { L"个加密条目", L" encrypted" }, { L" 个加密条目", L" encrypted" },
        { L"已解压：", L"Extracted: " },
        { L"解压完成。", L"Extraction complete." }, { L"正在解压 ", L"Extracting " },
        { L"说明", L"Note" }, { L"提示", L"Tip" }, { L"重要", L"Important" },
        { L"警告", L"Warning" }, { L"注意", L"Caution" },
        { L"不支持打开该文件类型：", L"Unsupported file type: " },
        { L"清空全部最近记录？", L"Clear all recent files?" },
        { L"路径不存在或无法访问。", L"The path does not exist." },
        { L"输入文件、文件夹或 ZIP 路径，支持 %USERPROFILE% 和相对路径。",
          L"Enter a file, folder or ZIP path. %USERPROFILE% and relative paths are supported." },
        { L"继续向下滑动加载更多", L"Scroll for more" },
        { L"已加载全部最近记录", L"All recent files loaded" },
        { L"打开一个文件后，最近记录会显示在这里。", L"Recent files will appear here." },
        { L"Ctrl+O   ·   也可以把文件直接拖进窗口", L"Ctrl+O   ·   or drop a file here" },
        { L"图片视图", L"Image view" }, { L"PDF 页面", L"PDF page" },
        { L"可拖动列宽 / 行高", L"Drag column / row edges" }, { L"画笔注释", L"Ink annotations" },
        { L"渲染 ", L"Rendered " }, { L"第 ", L"Page " }, { L" 页", L"" },

        { L"导出位图", L"Export bitmap" }, { L"输出分辨率", L"Output resolution" },
        { L"宽度 (px)", L"Width (px)" }, { L"实际输出：", L"Output: " },
        { L"快捷比例", L"Presets" }, { L"导出 PNG", L"Export PNG" },
        { L"正在生成预览…", L"Generating preview…" }, { L"已导出：", L"Exported: " },
        { L"导出失败", L"Export failed" }, { L"无法写入文件", L"Cannot write file" },
        { L"无法生成预览", L"Cannot generate preview" },
        { L"无法生成导出图片", L"Cannot generate image" },
        { L"无法创建导出画布", L"Cannot create export canvas" },
        { L"无法打开 PDF 文件", L"Cannot open PDF" }, { L"表格为空", L"Empty table" },
        { L"无法显示图片", L"Cannot show image" }, { L"无法解码图片", L"Cannot decode image" },
        { L"PDF 渲染不可用", L"PDF rendering unavailable" },
        { L"当前编码：", L"Encoding: " }, { L"文档语言：", L"Language: " },
        { L"编码不确定", L"Encoding uncertain" },

        { L"清空", L"Clear" }, { L"完成", L"Done" }, { L"宽度", L"Width" },
        { L"颜色", L"Color" }, { L"更多", L"More" },
        { L"翻译文件夹", L"Translations folder" }, { L"重新扫描", L"Rescan" },
        { L"无法打开文件", L"Cannot open file" }, { L"打开其他文件", L"Open another file" },
        { L"用系统默认程序打开", L"Open with system app" },
        { L"当前没有可导出的内容", L"Nothing to export" },
        { L"窗口尺寸无效", L"Invalid window size" }, { L"缩放比例无效", L"Invalid scale" },
        { L"导出尺寸超出范围", L"Export size out of range" },
        { L"无法写入文件", L"Cannot write file" },
        { L"表格为空", L"Empty table" }, { L"无法显示图片", L"Cannot show image" },
        { L"PDF 渲染不可用", L"PDF rendering unavailable" },
        { L"已恢复默认", L"Restored defaults" },
        { L"本地配置和历史记录已清空。", L"Local configuration and history cleared." },
        { L"配置路径已复制到剪贴板。", L"Configuration path copied." },
        { L"配置内容已复制到剪贴板。", L"Configuration copied." },
        { L"无法读取配置文件。", L"Cannot read configuration." },
        { L"已恢复全部默认快捷键。", L"All shortcuts restored." },
        { L"注册未完全成功，请检查当前用户权限。", L"Registration incomplete. Check permissions." },
        { L"已将 LitheMD 加入“打开方式”。", L"LitheMD added to Open With." },
        { L"已将 LitheMD 加入“打开方式”，并注册了选中的 ", L"LitheMD added to Open With; registered " },
        { L"新建画布默认尺寸已设为 ", L"Default canvas size set to " },
        { L"新建画布默认使用白色背景。", L"New canvas uses a white background." },
        { L"新建画布默认使用黑色背景。", L"New canvas uses a black background." },
        { L"当前文档有未保存的笔记。", L"This document has unsaved notes." },
        { L"有标签页含未保存的笔记。", L"Some tabs have unsaved notes." },
        { L"保存", L"Save" }, { L"不保存", L"Discard" }, { L"取消", L"Cancel" },

        { L"    ·    编码不确定", L"    ·    Encoding uncertain" },
        { L"    ·    已修复乱码", L"    ·    Encoding repaired" },
        { L"    文档语言：", L"    Language: " },
        { L"  ·  JSON 树", L"  ·  JSON tree" },
        { L"  ·  PDF 页面", L"  ·  PDF page" },
        { L"  ·  笔记未保存", L"  ·  Notes unsaved" },
        { L"  ·  笔记文件异常", L"  ·  Notes file error" },
        { L"  ·  编码待确认", L"  ·  Encoding uncertain" },
        { L"  ·  打开失败", L"  ·  Open failed" },
        { L"  ·  画笔注释", L"  ·  Ink annotations" },
        { L"  ·  可拖动列宽 / 行高", L"  ·  Drag column / row edges" },
        { L"  ·  图片视图", L"  ·  Image view" },
        { L"  ·  渲染 ", L"  ·  Rendered " },
        { L"  ·  已修复乱码", L"  ·  Encoding repaired" },
        { L"  ·  源码", L"  ·  Source" },
        { L" 个段落", L" paragraphs" }, { L" 个工作表", L" sheets" },
        { L" 个节点", L" nodes" }, { L" 个块", L" blocks" },
        { L" 行", L" rows" }, { L" 种文件格式。", L" file formats." },
        { L"（先到先得）", L" (first come, first served)" },
        { L"”使用，请换一个。", L"\" is already in use. Choose another." },
        { L"…（源码过长，已截断显示）", L"… (source too long, truncated)" },
        { L"LitheMD 文档", L"LitheMD Document" }, { L"按组合键…", L"Press shortcut…" },
        { L"白色画布", L"White canvas" }, { L"黑色画布", L"Black canvas" },
        { L"保存笔记", L"Save notes" }, { L"保存笔记失败", L"Cannot save notes" },
        { L"笔记文件超过 64 MB，已跳过加载", L"Notes file over 64 MB; skipped" },
        { L"笔记文件格式无效", L"Invalid notes file" },
        { L"笔记文件属于其他文档", L"Notes belong to another document" },
        { L"撤销", L"Undo" }, { L"撤销画笔", L"Undo ink" },
        { L"重做画笔", L"Redo ink" }, { L"错误", L"Error" },
        { L"打开文件 - LitheMD", L"Open file - LitheMD" },
        { L"当前没有可保存的文档", L"No document to save" },
        { L"当前文档有未保存的笔记。\n\n是否保存后再继续？", L"This document has unsaved notes.\n\nSave before continuing?" },
        { L"有标签页含未保存的笔记。\n\n是否保存全部笔记后退出？", L"Some tabs have unsaved notes.\n\nSave all notes before quitting?" },
        { L"导出图片", L"Export image" }, { L"输出图片", L"Output image" },
        { L"到底部", L"Bottom" }, { L"回到顶部", L"Top" },
        { L"点击按键框后直接按组合键录制；Esc 取消，Backspace 清除；重复绑定会被忽略。", L"Click a key field, then press the shortcut. Esc cancels, Backspace clears; duplicates are ignored." },
        { L"该组合键不可用，请换一个。", L"Shortcut unavailable. Choose another." },
        { L"放大", L"Zoom in" }, { L"缩小", L"Zoom out" },
        { L"重置缩放", L"Reset zoom" }, { L"关闭标签", L"Close tab" },
        { L"韩文", L"Korean" }, { L"日文", L"Japanese" },
        { L"画笔模式", L"Ink mode" }, { L"画布 ", L"Canvas " },
        { L"恢复全部默认", L"Restore all defaults" }, { L"会话", L"Session" },
        { L"将删除本地配置、最近记录和配置缓存。已打开的文件不会被删除。\n\n是否继续？", L"Local settings, recent files and cache will be deleted. Open files are kept.\n\nContinue?" },
        { L"就绪  ·  拖入文件或按 Ctrl+O 打开", L"Ready  ·  Drop a file or press Ctrl+O" },
        { L"切换主题", L"Toggle theme" },
        { L"轻量本地文档阅读器（Markdown / JSON / Office / PDF / 图片）", L"Local document reader (Markdown / JSON / Office / PDF / images)" },
        { L"请先勾选要取消默认关联的文件格式。", L"Select the file types to clear defaults for." },
        { L"请先勾选要取消注册的文件格式。", L"Select the file types to unregister." },
        { L"请先勾选要设为默认的文件格式。", L"Select the file types to set as default." },
        { L"取消组合", L"Ungroup" }, { L"删除笔迹", L"Delete ink" },
        { L"上一页", L"Previous page" }, { L"下一页", L"Next page" },
        { L"输出", L"Output" }, { L"图片", L"Images" },
        { L"图片无法显示", L"Cannot show image" },
        { L"未绑定", L"Unbound" }, { L"文档导航", L"Document navigation" },
        { L"文件已被移动或删除。", L"The file was moved or deleted." },
        { L"无法创建 LitheMD 窗口。", L"Cannot create LitheMD window." },
        { L"无法创建画布窗口。", L"Cannot create canvas window." },
        { L"无法创建文档窗口。", L"Cannot create document window." },
        { L"无法打开该文件", L"Cannot open this file" },
        { L"无法打开快捷键设置窗口。", L"Cannot open shortcut settings." },
        { L"无法读取笔记文件", L"Cannot read notes file" },
        { L"无法确定笔记文件位置", L"Cannot locate notes file" },
        { L"无法替换笔记文件（可能被占用）", L"Cannot replace notes file (in use)" },
        { L"无法写入笔记文件", L"Cannot write notes file" },
        { L"无法重新加载该文件", L"Cannot reload this file" },
        { L"系统选择窗口未能完成。也可以在“Windows 默认应用”中调整。", L"The system picker did not finish. You can also adjust Windows defaults." },
        { L"向上滚动", L"Scroll up" }, { L"向下滚动", L"Scroll down" },
        { L"向左滚动", L"Scroll left" }, { L"向右滚动", L"Scroll right" },
        { L"已绑定：", L"Bound: " }, { L"已被“", L"Already used by \"" },
        { L"已打开 Windows 默认应用设置，请在系统界面中完成权限确认。", L"Windows default apps opened. Confirm there." },
        { L"已打开系统的默认应用权限设置，请勾选需要关联的格式。", L"Windows default app settings opened. Select the formats to associate." },
        { L"已恢复：", L"Restored: " },
        { L"已恢复默认（主题、滚轮速度、缩放、画笔工具按钮、背景图片、快捷键）。", L"Defaults restored (theme, wheel speed, zoom, pen tools, background image, shortcuts)." },
        { L"已将 LitheMD 加入“打开方式”。勾选文件格式后可同时注册对应的扩展名。", L"LitheMD added to Open With. Selected file types can be registered too." },
        { L"已清除：", L"Cleared: " },
        { L"已取消选中格式的“打开方式”注册。若某个格式当前仍由 LitheMD 打开，请在系统默认应用中改选其他应用。", L"Open With registration removed for selected types. If a type still opens with LitheMD, choose another app in Windows defaults." },
        { L"已取消选中格式的注册。Windows 不允许程序直接清除默认应用，请在系统设置中为这些格式选择其他应用。", L"Registration removed for selected types. Windows does not allow clearing defaults directly; choose another app in system settings." },
        { L"已在系统弹窗中完成选择。勾选“始终使用”即可设为默认。", L"Selection completed in the system dialog. Check \"Always use\" to set default." },
        { L"应用命令", L"Apply command" }, { L"荧光笔", L"Highlighter" },
        { L"粘贴笔迹", L"Paste ink" }, { L"重复绑定：", L"Duplicate: " },
        { L"组合笔迹", L"Group ink" }, { L"自选 ", L"Custom " },
        { L"所有支持的文件", L"All supported files" }, { L"所有文件", L"All files" },
        { L"JSON / Notebook / 对话记录", L"JSON / Notebook / Chat logs" },
        { L"表格", L"Tables" }, { L"纯文本 (*.txt)", L"Plain text (*.txt)" },
        { L"PNG 图片 (*.png)", L"PNG image (*.png)" },
        { L"所有文件 (*.*)", L"All files (*.*)" },
        { L"CSV 表格", L"CSV table" }, { L"Excel 97 工作簿", L"Excel 97 workbook" },
        { L"Excel 工作簿", L"Excel workbook" }, { L"PDF 文档", L"PDF document" },
        { L"Word 97 文档", L"Word 97 document" }, { L"Word 文档", L"Word document" },
        { L"对话记录", L"Chat log" }, { L"文档", L"Document" },
        { L"读取文件失败", L"Cannot read file" }, { L"写入文件失败", L"Cannot write file" },
        { L"无法读取文件大小", L"Cannot read file size" },
        { L"文件超过 512 MB，已停止读取", L"File exceeds 512 MB; reading stopped" },
        { L"UTF-8 BOM 内容无效", L"Invalid UTF-8 BOM content" },
        { L"自动（UTF-8 优先）", L"Auto (UTF-8 first)" }, { L"未知编码", L"Unknown encoding" },
        { L"当前文件不支持编码调整", L"Encoding is not available for this file" },
        { L"文件过大，不支持预览", L"File too large to preview" },
        { L"文档为空", L"Document is empty" },
        { L"调色盘", L"Color palette" }, { L"预设颜色", L"Preset colors" },
        { L"新建", L"New" }, { L"确定", L"OK" }, { L"透明度", L"Opacity" },
        { L"主栏颜色 ", L"Toolbar colors " },
        { L"蓝白", L"Blue" }, { L"暖纸", L"Warm paper" }, { L"护眼", L"Eye care" },
        { L"输出设置", L"Output settings" }, { L"格式", L"Format" },
        { L"页面", L"Pages" }, { L"A4 分页", L"A4 pages" },
        { L"单张长页", L"Single long page" }, { L"清晰度", L"Quality" },
        { L"PDF 文件 (*.pdf)", L"PDF file (*.pdf)" }, { L"导出 PDF", L"Export PDF" },
        { L"已导出 PDF：", L"Exported PDF: " }, { L"已导出页面：", L"Exported pages: " },
        { L" 张", L" pages" }, { L"无法读取导出页面", L"Cannot read export page" },
        { L"页数过多，已取消导出", L"Too many pages; export cancelled" },
        { L"无法按 ", L"Cannot decode as " }, { L" 解码该文件", L" for this file" },
        { L"无法打开文件（错误 ", L"Cannot open file (error " },
        { L"无法写入文件（错误 ", L"Cannot write file (error " },
        { L" · 思考 ", L" · Thinking " }, { L" 个 Markdown / ", L" Markdown / " },
        { L" 个代码 / ", L" code / " }, { L" 个单元", L" cells" },
        { L" 个输出", L" outputs" }, { L" 条回复", L" replies" },
        { L" 条提问 / ", L" prompts / " }, { L" 条消息", L" messages" },
        { L"CSV 表格 · ", L"CSV table · " }, { L"CSV 内容为空", L"CSV is empty" },
        { L"CSV 中没有数据", L"No data in CSV" },
        { L"document.xml 中缺少 body", L"document.xml is missing body" },
        { L"Excel 97 工作簿 · ", L"Excel 97 workbook · " },
        { L"Excel 工作簿 · ", L"Excel workbook · " },
        { L"Excel 工作簿中没有工作表", L"No worksheets in Excel workbook" },
        { L"Excel 工作簿中没有可显示的数据", L"No displayable data in Excel workbook" },
        { L"Excel 文件中缺少 Workbook 流", L"Workbook stream missing in Excel file" },
        { L"JSON 结构", L"JSON structure" }, { L"JSON 解析失败：", L"JSON parse failed: " },
        { L"Notebook 解析失败：", L"Notebook parse failed: " },
        { L"notebook 中缺少 cells 数组", L"Notebook is missing cells array" },
        { L"OpenCode 对话记录 · ", L"OpenCode chat log · " },
        { L"OpenCode 会话", L"OpenCode session" },
        { L"PDF 使用了不支持的加密方式", L"PDF uses unsupported encryption" },
        { L"PDF 受密码保护", L"PDF is password protected" },
        { L"PDF 页面索引无效", L"Invalid PDF page index" },
        { L"PDF 引擎版本不兼容（缺少所需的 FPDF 接口）", L"Incompatible PDF engine (missing FPDF interface)" },
        { L"PDF 引擎错误 ", L"PDF engine error " }, { L"PDF 中没有页面", L"PDF has no pages" },
        { L"Word 文档中没有可显示的正文", L"No displayable content in Word document" },
        { L"Word 文件中缺少 Table 流", L"Table stream missing in Word file" },
        { L"Word 文件中缺少 WordDocument 流", L"WordDocument stream missing in Word file" },
        { L"按页显示", L"Page view" }, { L"本文档没有标题大纲", L"No headings in this document" },
        { L"不是有效的 Jupyter Notebook", L"Invalid Jupyter Notebook" },
        { L"不是有效的 OpenCode 会话记录", L"Invalid OpenCode session log" },
        { L"不是有效的 PDF 文件", L"Invalid PDF file" },
        { L"段落 ", L"Paragraph " }, { L"工具 · ", L"Tools · " },
        { L"工作簿中没有工作表", L"No worksheets in workbook" },
        { L"会话记录解析失败：", L"Chat log parse failed: " },
        { L"兼容显示", L"Compatibility view" }, { L"内核 ", L"Kernel " },
        { L"内置 PDF 引擎解压失败，无法打开 PDF 文件", L"Built-in PDF engine failed to decompress the PDF" },
        { L"你", L"You" }, { L"输出:\n", L"Output:\n" }, { L"输入:\n", L"Input:\n" },
        { L"输出公式", L"Output formula" },
        { L"数据较大，已截断为最多 3000 行 × 120 列", L"Large data truncated to 3000 rows × 120 columns" },
        { L"思考过程", L"Thinking" }, { L"未知错误", L"Unknown error" },
        { L"文件不存在或无法访问：\n", L"File missing or inaccessible:\n" },
        { L"文件超过 512 MB，LitheMD 已停止加载", L"File exceeds 512 MB; LitheMD stopped loading" },
        { L"文件过大或格式异常，内存不足，已停止解析", L"File too large or malformed; out of memory" },
        { L"文件解析失败，格式可能已损坏", L"Parse failed; the file may be corrupted" },
        { L"文件解析失败：", L"Parse failed: " },
        { L"文件无法打开或已损坏", L"Cannot open or file is corrupted" },
        { L"无法创建 PDF 渲染缓冲区", L"Cannot create PDF render buffer" },
        { L"无法从 Word 97 二进制格式中提取文本", L"Cannot extract text from Word 97 binary" },
        { L"无法打开 PDF：", L"Cannot open PDF: " },
        { L"无法读取 Excel 97 复合文档：", L"Cannot read Excel 97 compound document: " },
        { L"无法读取 Excel 工作簿", L"Cannot read Excel workbook" },
        { L"无法读取 PDF 页面", L"Cannot read PDF page" },
        { L"无法读取 Word 97 复合文档：", L"Cannot read Word 97 compound document: " },
        { L"无法读取 Word 文档", L"Cannot read Word document" },
        { L"无法加载 PDF 引擎（pdfium.dll，错误 ", L"Cannot load PDF engine (pdfium.dll, error " },
        { L"无法解码该文件", L"Cannot decode this file" },
        { L"无法解码这张图片，或当前系统不支持该图片格式", L"Cannot decode this image or format unsupported" },
        { L"无法解析该 Office 文档", L"Cannot parse this Office document" },
        { L"页面内容无法读取", L"Page content unreadable" },
        { L"以表格形式显示单元格内容", L"Show cell contents as table" },
        { L"这是旧版 Word 文档，LitheMD 会尽量保留正文内容；复杂版式可能无法完全还原。", L"Legacy Word document. LitheMD preserves text; complex layout may differ." },
    };
    return map;
}

void SetActiveMap(const std::map<std::wstring, std::wstring>* map) {
    g_pairs.clear();
    if (!map) return;
    for (auto& kv : *map) g_pairs.push_back(kv);
    std::sort(g_pairs.begin(), g_pairs.end(), [](const auto& a, const auto& b) {
        return a.first.size() > b.first.size();
    });
}

void ScanFolder(const std::wstring& dir,
                std::map<std::wstring, std::map<std::wstring, std::wstring>>& stringsByCode,
                std::vector<UiLanguage>& languages) {
    std::wstring pattern = PathJoin(dir, L"*.json");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring path = PathJoin(dir, fd.cFileName);
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(path, bytes)) continue;
        std::wstring enc;
        JNode root;
        if (!ParseJson(DecodeBytes(bytes, enc), root) || root.kind != JKind::Object) continue;
        std::wstring code = root.Get(L"code") ? root.Get(L"code")->Text() : L"";
        std::wstring name = root.Get(L"name") ? root.Get(L"name")->Text() : code;
        if (code.empty()) {
            code = fd.cFileName;
            size_t dot = code.rfind(L'.');
            if (dot != std::wstring::npos) code.resize(dot);
        }
        if (code.empty() || _wcsicmp(code.c_str(), L"zh-CN") == 0 ||
            _wcsicmp(code.c_str(), L"zh") == 0 || _wcsicmp(code.c_str(), L"en-US") == 0 ||
            _wcsicmp(code.c_str(), L"en") == 0)
            continue;
        JNode* strings = root.Get(L"strings");
        if (!strings || strings->kind != JKind::Object) continue;
        std::map<std::wstring, std::wstring> stringsMap;
        for (auto& m : strings->members)
            if (m.second.kind == JKind::String && !m.second.str.empty())
                stringsMap[m.first] = m.second.str;
        if (stringsMap.empty()) continue;
        if (stringsByCode.count(code)) continue;
        stringsByCode[code] = std::move(stringsMap);
        languages.push_back({ code, name.empty() ? code : name });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void EnsureLoaded() {
    if (g_loaded) return;
    g_loaded = true;
    g_languages = { { L"zh-CN", L"中文" }, { L"en-US", L"English" } };
    g_external.clear();
    std::wstring cfg = Settings::ResolveConfigPath();
    ScanFolder(PathJoin(PathDir(cfg), L"translations"), g_external, g_languages);
    ScanFolder(PathJoin(ExeDir(), L"translations"), g_external, g_languages);
    std::sort(g_languages.begin() + 2, g_languages.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

} // namespace

const std::vector<UiLanguage>& UiLanguages() {
    EnsureLoaded();
    return g_languages;
}

void ReloadUiLanguages() {
    g_loaded = false;
    EnsureLoaded();
    SetUiLanguage(g_code);
}

void SetUiLanguage(const std::wstring& code) {
    EnsureLoaded();
    g_code = code.empty() ? L"zh-CN" : code;
    if (_wcsicmp(g_code.c_str(), L"zh-CN") == 0 || _wcsicmp(g_code.c_str(), L"zh") == 0) {
        SetActiveMap(nullptr);
        g_code = L"zh-CN";
        return;
    }
    if (_wcsicmp(g_code.c_str(), L"en-US") == 0 || _wcsicmp(g_code.c_str(), L"en") == 0) {
        SetActiveMap(&EnglishMap());
        g_code = L"en-US";
        return;
    }
    auto it = g_external.find(g_code);
    if (it != g_external.end()) {
        SetActiveMap(&it->second);
    } else {
        SetActiveMap(&EnglishMap());
        g_code = L"en-US";
    }
}

const std::wstring& CurrentUiLanguage() {
    return g_code;
}

std::wstring TranslateUi(const std::wstring& text) {
    if (g_pairs.empty()) return text;
    for (auto& pair : g_pairs) {
        if (text == pair.first) return pair.second;
    }
    // Only allow substring substitution for keys that act as prefixes or
    // suffixes ("当前编码：", "第 ", " 页", "已导出：").  Replacing short
    // single-character keys inside unrelated text would corrupt it.
    auto composable = [](const std::wstring& key) {
        if (key.empty()) return false;
        if (key.front() == L' ') return true;
        wchar_t back = key.back();
        return back == L' ' || back == L'\uFF1A' || back == L':' ||
               back == L'\u2026' || back == L'\uFF0C';
    };
    std::wstring out = text;
    for (auto& pair : g_pairs) {
        if (!composable(pair.first)) continue;
        size_t pos = 0;
        while ((pos = out.find(pair.first, pos)) != std::wstring::npos) {
            out.replace(pos, pair.first.size(), pair.second);
            pos += pair.second.size();
        }
    }
    return out;
}
