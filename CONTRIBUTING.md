# 贡献约定

## 提交署名

**每条提交必须署提交者本人的身份**，不要把提交记到协作者账号名下。

原因很直接：GitHub 按提交里的邮箱把提交归到账号。署名写错，代码就挂到别人名下——包括贡献图、个人主页和 `git blame`。

协作者在本仓库留下的提交，不允许使用以下身份：

```
Shenyun-Z <121408869+Shenyun-Z@users.noreply.github.com>
```

## 一次性配置

克隆后先确认身份，再安装仓库内置钩子：

```powershell
git config --local user.name  "whylantern"
git config --local user.email "3778216901@qq.com"
git config --local -l | Select-String user      # 确认没有残留
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install-git-hooks.ps1
```

`tools/install-git-hooks.ps1` 会把 `core.hooksPath` 指向仓库内的 `.githooks`。之后 `pre-commit` 会检查 `user.email`：一旦是上面的协作者身份，提交直接被拒绝，并打印修正命令。

macOS / Linux / Git Bash 手动等价配置：

```bash
git config --local core.hooksPath .githooks
chmod +x .githooks/pre-commit
```

## 关于绕过

`git commit --no-verify` 可跳过钩子。钩子是防呆，不是权限控制；如果确实需要以协作者身份提交（例如对方明确要求代为提交），用：

```bash
LITHEMD_ALLOW_SELF=1 git commit ...
```

## 如果已经从错误的身份提交过

- 还没推送：`git commit --amend --author="你的名字 <你的邮箱>" --reset-author`，或 `git reset --soft HEAD~1` 后重新提交。
- 已经推送：需要改写历史（`git filter-repo` 或 `git rebase -i`），并由你本人执行，改完 force push、重建相关 tag。
