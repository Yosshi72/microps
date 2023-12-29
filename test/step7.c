#include <stdio.h>
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

int
main(int argc, char *argv[])
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
    while (!terminate) { // Ctrl+Cが押されるとterminate=1となり，loopを抜ける
        if (net_device_output(dev, NET_PROTOCOL_TYPE_IP, test_data, sizeof(test_data), NULL) == -1) { // 1secごとにdeviceにpacketを書き込み
            errorf("net_device_output() failure");
            break;
        }
        sleep(1);
    }
    net_shutdown(); // protocol stackの停止
    return 0;
}