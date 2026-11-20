# 第三方组件

## PDFium

- 项目：https://pdfium.googlesource.com/pdfium/
- 许可：BSD 3-Clause
- 用途：PDF 页面渲染
- 本仓库位置：`vendor/pdfium/pdfium.dll`、`vendor/pdfium/fpdfview.h`、`vendor/pdfium/LICENSE`
- 版本：Chromium PDFium build 8152（来自 bblanchon/pdfium-binaries）

## miniz

- 项目：https://github.com/richgel999/miniz
- 许可：MIT / public domain（见 `vendor/miniz.LICENSE`）
- 用途：DOCX / XLSX 的 ZIP 解压

## w64devkit / GCC / MinGW-w64

- 项目：https://github.com/skeeto/w64devkit
- 版本：2.11.0（GCC 16.2.0）
- 用途：构建时使用。仓库内 `tools\w64devkit` 是精简副本，只保留编译 LitheMD 所需的 `bin`、`include`、`lib` 和 `libexec`。
- 许可：w64devkit 随附的 `README.md`、`COPYING.MinGW-w64-runtime.txt`、`VERSION.txt` 已保留在 `tools\w64devkit`。GCC 及其运行库按其各自的 GPL / runtime exception 许可分发。
- 说明：目标机器运行 `dist\lithemd.exe` 时不需要 MinGW/GCC。

## 示例文件

- `samples/02_python.ipynb`、`samples/demo.md`、`samples/demo.json`、`samples/latex.md`、`samples/people.csv`：随项目用于演示各格式。
- `samples/sample.pdf`：由本项目使用 Microsoft Edge 无头打印生成。
- `samples/sample.docx` / `sample.xlsx`：由本项目生成的最小 OOXML 示例。
- `samples/legacy.doc` / `legacy.xls`：来自 Apache POI 测试数据（Apache License 2.0），仅用于兼容性演示。

## 字体

LitheMD 不随包分发字体。运行时会请求系统字体：

- Microsoft YaHei UI / Segoe UI（正文与界面）
- Consolas（代码）
- Cambria Math（数学公式）
- Segoe MDL2 Assets（工具栏图标）

如果系统缺少某个字体，DirectWrite 会自动回退到可用的系统字体。
