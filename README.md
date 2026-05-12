# rkd - RK3568 Process Monitor Daemon

## 项目简介

rkd 是一个面向 RK3568 嵌入式 Linux 平台的进程监控守护进程。它能够托管一组用户配置的进程，在进程异常退出或正常结束时自动重启，并通过 Unix Domain Socket 对外提供 start / stop / restart / status / list 五种控制命令。项目使用纯 C 语言编写，仅依赖标准库和 POSIX 接口，不引入任何第三方库，适合资源受限的嵌入式环境。

## 技术架构

### 守护进程（daemon.c）

采用标准的**两次 fork + setsid**方案将自身变为守护进程：

1. 第一次 fork：父进程退出，子进程成为孤儿进程，由 init 收养
2. 子进程调用 `setsid()` 创建新会话，脱离原控制终端
3. 第二次 fork：第一代子进程退出，最终的守护进程不再是会话组长，无法重新关联控制终端
4. 关闭 stdin/stdout/stderr，重定向到 `/dev/null`
5. 切换工作目录到 `/tmp`，写入 PID 文件 `/tmp/rkd.pid`

### 进程管理（process_mgr.c）

负责被托管进程的完整生命周期管理：

- **启动**：`fork()` 创建子进程，通过 `execvp()` 执行目标命令
- **停止**：先发送 `SIGTERM`（优雅退出），等待 3 秒；若进程未退出则发送 `SIGKILL` 强制终止
- **监控**：独立线程中运行 `waitpid(WNOHANG)` 非阻塞轮询，检测到子进程退出后立即触发自动重启
- **重启控制**：可配置最大重启次数（`-1` 表示无限）和重启间隔延迟，超过上限后放弃重启并告警
- **线程安全**：使用 `pthread_mutex_t` 保护进程管理数据结构，sleep 等待期间释放锁避免阻塞 IPC 线程

### IPC 通信（ipc.c）

- **传输层**：Unix Domain Socket（`/tmp/rkd.sock`），相比 TCP 回环，UDS 在内核态完成数据传递，零拷贝，延迟更低
- **多路复用**：`epoll` 管理监听 socket 和所有客户端连接，Level-Triggered 模式，支持最多 16 个并发客户端
- **应用协议**：定长二进制帧，请求 64 字节 `[1 字节命令][63 字节进程名]`，响应 128 字节 `[4 字节返回码][124 字节消息]`
- **命令集**：CMD_START(1)、CMD_STOP(2)、CMD_RESTART(3)、CMD_STATUS(4)、CMD_LIST(5)

### 日志模块（log.c）

- 日志同时写入 `/tmp/rkd.log`（行缓冲模式，实时落盘）和标准错误输出
- 带时间戳和分级的格式化输出：`[2026-05-12 10:23:45][INFO] message`
- 提供三个宏简化调用：`LOG_INFO()`、`LOG_WARN()`、`LOG_ERROR()`

## 核心技术点

### 守护进程实现（两次 fork 原理）

第一次 fork 后父进程退出，子进程调用 `setsid()` 成为新会话的 leader 并脱离控制终端。但会话 leader 仍有权重新获取控制终端（打开终端设备时），因此进行第二次 fork——孙子进程不再是会话 leader，彻底丧失获取控制终端的能力。这种做法是 System V 守护进程的标准范式。

### epoll 事件驱动

`epoll` 是 Linux 特有的 I/O 多路复用机制，与 `select`/`poll` 的核心区别：

- **O(1) 就绪事件获取**：`epoll_wait` 直接返回就绪 fd 列表，不像 `select` 需要遍历整个 fd 集合（O(n)）
- **无 fd 数量限制**：`select` 默认上限 1024，`epoll` 仅受系统资源限制
- **事件驱动而非轮询**：fd 通过 `epoll_ctl` 注册到内核事件表，就绪时内核主动通知，避免每次调用重复传入 fd 集合

在 rkd 中，epoll 以 1 秒超时循环运行，既能及时响应客户端请求，又能在超时时检查退出标志实现优雅停机。

### Unix Domain Socket

选择 UDS 而非 TCP 回环（127.0.0.1）的原因：

- **性能**：UDS 在内核空间完成数据传输，无需经过网络协议栈，延迟更低、吞吐更高
- **安全**：UDS 的访问控制基于文件系统权限（`/tmp/rkd.sock` 的 owner/group/mode），无需额外的网络层认证
- **简化部署**：不使用端口号，不占用 IP 地址资源，无需防火墙配置

### waitpid(WNOHANG) 非阻塞检测

监控线程以 1 秒间隔调用 `waitpid(pid, &status, WNOHANG)` 轮询所有子进程：

- `WNOHANG` 标志使 `waitpid` 立即返回而非阻塞等待，返回 0 表示子进程仍在运行
- 返回正值表示子进程状态变更，通过 `WIFEXITED(status)` 和 `WIFSIGNALED(status)` 判断退出原因
- 多线程环境下，`waitpid` 指定特定 pid 而非 `waitpid(-1, ...)`，避免与 IPC 线程中 `stop_process` 的 waitpid 竞争

### SIGTERM / SIGKILL 两阶段停止

停止一个进程时采用递进策略：

1. **SIGTERM**（可捕获）：通知进程主动退出，进程有机会执行清理（释放资源、刷新缓冲、删除临时文件等）
2. 等待 3 秒，若进程仍未退出，发送 **SIGKILL**（不可捕获）：内核直接终止进程，确保停止操作不会无限阻塞

这种设计兼顾了"优雅退出"和"强制终止"两者的优点。

## 编译运行

### 编译

```sh
# 本地编译
make

# 交叉编译（ARM64）
CC=aarch64-linux-gnu-gcc make

# 安装到系统路径
make install
```

编译选项：`-Wall -Wextra -g -pthread -D_GNU_SOURCE`

### 启动守护进程

```sh
# 使用默认配置 /etc/rkd.conf
./rkd

# 指定配置文件（建议用绝对路径）
./rkd -c $(pwd)/config/example.conf

# 查看版本
./rkd -v
```

启动后守护进程会将自身 PID 写入 `/tmp/rkd.pid`，可通过该文件停止：

```sh
kill $(cat /tmp/rkd.pid)
```

### 客户端命令

```sh
./rkctl start  <进程名>    # 启动进程
./rkctl stop   <进程名>    # 停止进程
./rkctl restart <进程名>   # 重启进程（停→延迟→启）
./rkctl status <进程名>    # 查看单进程状态
./rkctl list               # 列出所有进程
```

## 配置文件格式

每行定义一个托管进程，格式：

```
进程名=启动命令,最大重启次数,重启延迟秒数
```

| 字段 | 说明 |
|------|------|
| 进程名 | 唯一标识，用于 rkctl 控制命令 |
| 启动命令 | 可执行文件路径及参数，空格分隔 |
| 最大重启次数 | 崩溃后最多自动重启几次，`-1` 表示无限 |
| 重启延迟 | 两次重启之间的等待秒数，避免快速循环 |

示例：

```
# 测试进程：/bin/sleep 每30秒退出，最多重启3次，间隔2秒
test_sleep=/bin/sleep 30,3,2

# Nginx：前台运行模式，最多重启10次
nginx=/usr/sbin/nginx -g "daemon off;",10,3

# 自定义应用：最多重启5次
myapp=/root/myapp/app,5,1

# 无限重启（谨慎使用）
watchdog=/usr/bin/watchdog,-1,2
```

以 `#` 开头的行为注释。

## 日志

### 路径

`/tmp/rkd.log`

### 格式

```
[YYYY-MM-DD HH:MM:SS][级别] 消息内容
```

### 示例

```
[2026-05-12 08:00:01][INFO] rkd v1.0.0 starting...
[2026-05-12 08:00:01][INFO] daemon started successfully, pid=2183
[2026-05-12 08:00:01][INFO] added process: test_sleep cmd=/bin/sleep 30 max_restart=3 delay=2
[2026-05-12 08:00:01][INFO] loaded 1 processes from /home/root/rkd/config/example.conf
[2026-05-12 08:00:01][INFO] process test_sleep started, pid=2185, cmd=/bin/sleep 30
[2026-05-12 08:00:31][INFO] process test_sleep exited with code 0
[2026-05-12 08:00:33][INFO] process test_sleep restarted (count=1, exit_code=0)
[2026-05-12 08:01:03][INFO] process test_sleep exited with code 0
[2026-05-12 08:01:05][INFO] process test_sleep restarted (count=2, exit_code=0)
```

日志同时输出到标准错误（守护进程模式下重定向到 `/dev/null`，前台调试时可直接在终端查看）。

## 测试验证

使用 `config/example.conf` 中配置的 `test_sleep=/bin/sleep 30,3,2` 完成以下功能验证：

### 1. 进程启动和监控

```sh
./rkd -c $(pwd)/config/example.conf
./rkctl list
# 输出: test_sleep(running,2185)
```

### 2. 自动重启

等待 30 秒后 `/bin/sleep` 正常退出（exit code 0），守护进程在 2 秒延迟后自动重启：

```sh
tail -f /tmp/rkd.log | grep restarted
# [INFO] process test_sleep restarted (count=1, exit_code=0)
# [INFO] process test_sleep restarted (count=2, exit_code=0)
# [INFO] process test_sleep restarted (count=3, exit_code=0)
```

### 3. 重启次数上限

第 4 次退出后 `restart_count` 达到 4，超过 `max_restart=3`，守护进程放弃重启：

```sh
tail -f /tmp/rkd.log | grep "giving up"
# [WARN] process test_sleep reached max restarts (3), giving up
./rkctl status test_sleep
# 输出: name=test_sleep status=stopped pid=-1 restarts=4
```

### 4. IPC 控制命令

```sh
./rkctl start test_sleep     # 手动启动已停止的进程
./rkctl status test_sleep    # 确认状态为 running
./rkctl restart test_sleep   # 重启（停→延迟→启，restart_count 归零）
./rkctl stop test_sleep      # 停止进程
./rkctl list                 # 确认状态为 stopped
```

### 5. 多客户端并发

同时打开多个终端执行 `rkctl list` / `rkctl status`，状态保持一致，不会因监控线程正在重启进程而出现数据不一致。

## 目录结构

```
rkd/
  main.c              程序入口，参数解析，模块编排
  src/
    daemon.c/h        守护进程实现
    process_mgr.c/h   进程管理（启动/停止/重启/监控）
    ipc.c/h           Unix Socket + epoll IPC
    log.c/h           日志模块
  client/
    rkctl.c           命令行控制客户端
  config/
    example.conf      配置文件示例
  Makefile
  README.md
```

## 运行文件

| 路径 | 说明 |
|------|------|
| `/tmp/rkd.pid` | 守护进程 PID 文件 |
| `/tmp/rkd.sock` | Unix Domain Socket |
| `/tmp/rkd.log` | 日志文件 |
