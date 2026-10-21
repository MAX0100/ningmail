# NingMail

NingMail 是一个基于 Qt 6 / QML 的桌面邮件客户端。

## 构建

配置时把 `<你的 Qt 6.8 路径>` 替换为本机 Qt 6.8 的安装目录：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=<你的 Qt 6.8 路径>
cmake --build build
```

## 测试

```bash
ctest --test-dir build --output-on-failure
./build/tests/unit/ningmail_tests
```

无显示环境下可运行界面冒烟测试：

```bash
QT_QPA_PLATFORM=offscreen ./build/app/ningmail --smoke-test
```
