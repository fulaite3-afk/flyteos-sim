# 贡献指南

感谢您对 FlyteOS 的关注！我们欢迎任何形式的贡献。

## 如何参与

### 报告问题
- 使用 GitHub Issues 报告 Bug 或功能建议
- 描述清楚问题环境和复现步骤
- 附上截图或日志会有很大帮助

### 代码贡献
1. Fork 本仓库
2. 创建特性分支
   ```bash
   git checkout -b feature/your-feature-name
   ```
3. 编写代码并测试
4. 提交更改
   ```bash
   git commit -m 'Add: 新增xxx功能'
   ```
5. 推送到您的 Fork
   ```bash
   git push origin feature/your-feature-name
   ```
6. 创建 Pull Request

### 提交信息规范

推荐使用以下前缀：
- `Add:` 新增功能
- `Fix:` 修复Bug
- `Update:` 更新现有功能
- `Refactor:` 代码重构
- `Docs:` 文档更新
- `Test:` 测试相关

## 开发环境

```bash
# 克隆
git clone https://github.com/fulaite3-afk/flyteos-sim.git

# Web模拟器
cd simulator
python -m http.server 8080

# 飞控固件 (需要 ARM 工具链)
# 参考 src/README.md
```

## 行为准则

请尊重所有参与者，保持友好和专业的交流。

---

有问题？发邮件至 contact@flyteos-sim.org 或创建 Issue。
