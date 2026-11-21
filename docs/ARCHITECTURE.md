# LitheMD 架构与新增模块说明

> 对应 2.4.0.0 的分层要求。全局约束见 `docs/REQUIREMENTS.md`。
> 本文件只描述代码边界、数据流和扩展方式，不重复产品文案。

## 1. 分层与目录

当前仍保持单个 `lithemd.exe`，但新增功能按三层放置：

```text
UI 层
  app.cpp                 主窗口、标签、命令、设置窗口
  view.cpp / view_ink.cpp 文档布局、绘制、注释
  workspace_view.cpp      工作区树绘制与交互
  archive_window.cpp      ZIP 浏览、预览、解压窗口
  dialogs.cpp             路径输入、密码输入、文件夹选择

Core 层
  path_util.cpp           用户路径解析
  workspace_model.cpp     懒加载目录树
  archive_reader.cpp      ZIP 目录、解密、解压
  file_watch.cpp          目录变化监听
  reading_positions.cpp   按文档保存滚动位置
  file_policy.cpp         不支持二进制类型特判
  workspace_model.cpp     工作区历史与懒加载目录树
  common.cpp              Settings/Recent/路径/编码等基础能力

Platform / 第三方
  Direct2D/DirectWrite、WIC、BCrypt、Win32 文件系统
  vendor/miniz.c          仅 Deflate/内存解压
```

规则：

- Core 模块不包含 `HWND`，不绘制、不调用 `InvalidateRect`。
- UI 不重复解析 ZIP、密码或路径；统一调用 Core。
- 新功能优先新增文件；`app.cpp` 只保留编排和窗口生命周期。
- 所有模块静态链接，不新增运行时 DLL。

## 2. 关键模块

### 2.1 PathResolver（`path_util.*`）

- 处理引号、`file://`、`%ENV%`、`~`、相对路径和尾部斜杠。
- 相对路径以当前文档所在目录或工作区根目录为基准。
- 只做路径规范化，不访问网络、不执行命令。

### 2.2 WorkspaceModel（`workspace_model.*`）

- 根目录按需枚举；子目录只有在展开时才读取。
- 目录优先、文件其次，名称忽略大小写排序。
- 单目录最多 4000 项；重解析点不递归，防止目录环。
- 返回扁平的 `WorkspaceRow` 列表，UI 只负责绘制。
- 不监听文件系统；刷新由 UI 显式触发。

### 2.3 ArchiveReader（`archive_reader.*`）

职责：

- 解析 ZIP/ZIP64 中央目录；
- 支持 Stored、Deflate；
- 支持 ZipCrypto 与 WinZip AES-128/256；
- 单条读取、全部解压、路径安全校验。
- 所有条目（包括文件夹条目）在解压前都校验名称和目标路径；Deflate 使用带输出上限的流式回调。

安全边界：

| 项目 | 限制 |
|---|---|
| 单条目解压后 | 512 MB |
| 总解压大小 | 4 GB |
| 条目数量 | 100000 |
| 压缩比 | 20000（且大于 16 MB 时检查） |
| 路径 | 拒绝绝对路径、盘符、UNC、`..`、ADS、设备名、控制字符 |
| 覆盖 | 默认拒绝；UI 明确询问后才覆盖 |

密码只保存在 `ArchiveWindow` 的内存字段中，不写入配置、最近记录或日志。
ZipCrypto 密码优先按 UTF-8 尝试，再按系统 ANSI 尝试；AES 使用 UTF-8。

### 2.4 FileWatcher（`file_watch.*`）

- 使用 `ReadDirectoryChangesW` 监听当前文档所在目录。
- 变化合并 180 ms 后向主窗口发送消息。
- 主窗口比较文件时间：
  - 无未保存笔记：自动重载；
  - 有未保存笔记：先标记外部变化，切换/退出时提示；
  - 文件被删除：保留标签并显示错误。
- 只监听一个目录，不递归、不建立索引。

### 2.5 FilePolicy（`file_policy.*`）

- 在 `LoadDocument` 读取文件之前拒绝不支持的可执行/系统二进制类型。
- `SourceTextForError` 对同一名单直接返回空文本，避免错误页再次读取。
- 支持格式（PDF、Office、图片、ZIP）不经过该拦截。
- 名单集中在 Core 层，后续新增格式时只修改一处。

### 2.6 ReadingPositions（`reading_positions.*`）

- 按文档绝对路径保存 0–1 的滚动比例，不保存像素位置。
- 配置文件与 `config.json` 同目录：`reading-positions.json`。
- 最多保留 500 条，按更新时间淘汰最旧记录。
- 文档滚动到顶部或没有可滚动内容时删除记录。
- 切换标签、关闭标签、退出应用时写入；打开文档后由 `DocView::RestoreScrollFraction` 恢复。

### 2.7 UI 组件

- `WorkspaceView`：左侧工作区树，展开/折叠、活动文件高亮、刷新和关闭。
- 文件菜单：工具栏和欢迎页共用一个“打开 ▾”菜单；右上角“文件 ▾”提供打开文件夹、显示/隐藏工作区和刷新。
- 最近工作区：设置保存最多 20 个根目录，欢迎页显示最近 3 个。
- `ArchiveWindow`：独立顶层窗口，列表、预览、单条解压、全部解压、密码提示。
- `ShowTextInput` / `PickFolder`：共用原生输入与选择器，不引入第三方 UI。

## 3. 数据流

### 打开普通文件

```text
命令/拖放/最近记录
  -> MainWindow::OpenFile
  -> LoadDocument (Core)
  -> DocView::SetDocument
  -> 记录最近文件 + 启动 FileWatcher
```

### 打开 ZIP

```text
OpenFile(.zip)
  -> MainWindow::OpenArchive
  -> ArchiveWindow::Create
  -> ArchiveReader::Open（只读目录，不展开数据）
  -> 预览：密码 -> Read -> 临时文件 -> 主窗口标签
  -> 解压：后台线程 -> ExtractAll -> 进度消息 -> UI
```

临时预览目录：

```text
%LOCALAPPDATA%\lithemd\preview\<archive-hash>\<index>-<safe-name>
```

启动时清空整个 `preview` 目录。相同归档条目重复预览会复用同一临时路径，避免重复标签。

### 工作区

```text
OpenWorkspace
  -> WorkspaceModel::SetRoot（只枚举第一层）
  -> WorkspaceView 绘制
  -> 点击目录：Toggle -> LoadChildren
  -> 点击文件：OpenFile
```

## 4. 变更约束

1. 新增解析、解压、目录枚举、路径处理：必须放 Core 文件。
2. 新增窗口或 Canvas 绘制：放独立 UI 文件，通过回调通知主窗口。
3. 阻塞操作：解压必须后台线程；密码校验和大文件读取需限制大小。
4. 所有新增用户文案：中文 + 英文映射；Canvas 文本通过 `TranslateUi`。
5. 涉及文件系统时使用临时目录测试，不污染真实最近记录和笔记。
6. 版本发布后按 `REQUIREMENTS.md` 清空旧任务表。

## 5. 已知边界与后续

- 分屏为同一文档的独立视图；Diff 尚未实现，已列为后续任务。
- 密码 ZIP 已支持 ZipCrypto 与 AES；不支持分卷 ZIP 和 ZIP64 超大文件。
- 工作区不递归监听，不提供文件操作（重命名/删除）。
- PDF 仍为栅格显示，不提供文本搜索。
- 不引入 Mermaid/图表、WYSIWYG、插件运行时或 WebView。
