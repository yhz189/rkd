# rkd Makefile
# RK3568 Process Monitor Daemon

CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread -D_GNU_SOURCE
LDFLAGS = -pthread

# 安装目录
PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin

# 源文件
SRC     = src/log.c src/daemon.c src/process_mgr.c src/ipc.c
MAIN    = main.c
CLIENT  = client/rkctl.c

# 目标
TARGET  = rkd rkctl

.PHONY: all clean install

all: $(TARGET)

# rkd 守护进程
rkd: $(MAIN) $(SRC) src/log.h src/daemon.h src/process_mgr.h src/ipc.h
	$(CC) $(CFLAGS) -o $@ $(MAIN) $(SRC) $(LDFLAGS)

# rkctl 控制客户端
rkctl: $(CLIENT)
	$(CC) $(CFLAGS) -o $@ $(CLIENT) $(LDFLAGS)

# 安装到系统路径
install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 rkd  $(DESTDIR)$(BINDIR)/rkd
	install -m 755 rkctl $(DESTDIR)$(BINDIR)/rkctl

# 清理编译产物
clean:
	rm -f $(TARGET)

# 彻底清理（包括PID文件和socket文件）
distclean: clean
	rm -f /tmp/rkd.pid /tmp/rkd.sock /tmp/rkd.log
