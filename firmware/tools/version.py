# PlatformIO 编译前脚本：生成固件版本号 FW_VERSION
# 优先用环境变量 FW_VERSION（发布时 CI 设为标签名，如 v0.5.0），
# 否则用 git describe（如 v0.5.0-3-gabc1234，或没有标签时的提交号），都没有就是 "dev"。
# 只有 vX.Y.Z 这样的正式版本号才会参与自动升级。
import os
import subprocess

Import("env")


def git_version():
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env.subst("$PROJECT_DIR"), stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return ""


version = os.environ.get("FW_VERSION") or git_version() or "dev"
print("FW_VERSION =", version)
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
