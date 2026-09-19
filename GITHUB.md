# 用 GitHub Actions 编译（android15-6.6 GKI）

workflow 文件：`.github/workflows/build.yml`，显示名 **Build pte_track.ko (android15-6.6 GKI)**。

## 步骤

1. 新建空仓库（例如 `pte_track`），把本目录**所有文件** push 上去（包含 `.github` 目录）。
2. 打开仓库 **Actions** 页 → 左侧选 *Build pte_track.ko (android15-6.6 GKI)* → 点 **Run workflow**。
   两个输入默认已按设备填好，直接跑即可：
   - `kernel_sublevel` = `118`（对应 6.6.118）
   - `localversion` = `-android15-8-g93e223c276e7-abogki-4k`
   设备 `uname -r` 变了，就改这两个再跑。
3. 等待构建完成。流程：clone GKI 源码 → 固定版本号 → 完整编译内核（生成带 CRC 的 Module.symvers）→ 编译模块 → 校验 vermagic。
   首次约 1~3 小时（4 核 runner，ThinLTO 内核编译较慢，属正常）。
4. 完成后点进该次 run → 页面底部 **Artifacts** → 下载 `pte_track-ko`（zip）→ 解压得到 `pte_track.ko`。
5. 把 `pte_track.ko` 与 `insmod_pte.sh` 推到手机同目录，`su` 下执行 `sh insmod_pte.sh`。

> push 到 main / master 会自动触发一次构建（很耗时）。不想自动跑：删掉 build.yml 里的 `push:` 整段。

## push 命令参考

```sh
cd pte_track_fixed
git init
git add .
git commit -m "pte_track for 6.6.118-android15"
git branch -M main
git remote add origin https://github.com/<你的用户名>/pte_track.git
git push -u origin main
```

## 排错

- **kernelrelease mismatch**：输入与目标版本串不一致，改输入重跑。
- **vermagic 不匹配（modinfo 校验失败）**：`localversion` 没抄全（注意开头的 `-`），或 `uname -r` 里还有别的段没写进去。
- **模块链接 undefined symbol**：内核树没编译完（`Module.symvers` 缺失），重跑即可；反复出现就检查构建日志里内核 `make` 是否失败。
- **clang 相关报错**：把安装行里的 `clang lld llvm` 换成 `clang-17 lld-17 llvm-17` 重试。
