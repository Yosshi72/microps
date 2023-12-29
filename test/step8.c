#include <stdio.h>
#include <stddef.h>
#include <signal.h>
#include <unistd.h>

#include "util.h"
#include "net.h"
#include "ip.h"

#include "driver/loopback.h"

#include "test.h"

static volatile sig_atomic_t terminate;

static void
on_signal(int s)
{
    (void)s;
    terminate = 1;
}

static int
setup(void) // protocol stackの初期化，デバイス登録と起動，を行う関数
{
    struct net_device *dev;
    struct ip_iface *iface;

    signal(SIGINT, on_signal); // signal handlerの設定．Ctrl+Cで終了するように
    if (net_init() == -1) { // protocol stackを初期化
        errorf("net_init() failure");
        return -1;
    }
    dev = loopback_init(); // loopback deviceの初期化．登録まで完了
    if (!dev) {
        errorf("loopback_init() failure");
        return -1;
    }
    iface = ip_iface_alloc(LOOPBACK_IP_ADDR, LOOPBACK_NETMASK); // ip interfaceの生成
    if (!iface) {
        errorf("ip_iface_alloc() failure");
        return -1;
    }
    if (ip_iface_register(dev, iface) == -1) { // ip interfaceの登録
        errorf("ip_iface_register() failure");
        return -1;
    }
    if (net_run() == -1) { // protocol stackを起動
        errorf("net_run() failure");
        return -1;
    }
    return 0;
}

static void
cleanup(void)
{
    net_shutdown();
}

int
main(int argc, char *argv[])
{
    ip_addr_t src, dst;
    size_t offset = IP_HDR_SIZE_MIN;

    if (setup() == -1) {
        errorf("setup() failure");
        return -1;
    }
    ip_addr_pton(LOOPBACK_IP_ADDR, &src); // ip addressをstr->binaryと変換
    dst = src; // dstはsrcと同じアドレス

    while (!terminate) { // Ctrl+Cが押されるとterminate=1となり，loopを抜ける
        if (ip_output(1, test_data + offset, sizeof(test_data) - offset, src, dst) == -1) { // 1secごとにdeviceにpacketを書き込み
            errorf("ip_output() failure");
            break;
        }
        sleep(1);
    }
    cleanup(); // protocol stackの停止
    return 0;
}