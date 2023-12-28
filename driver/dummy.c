#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

#include "util.h"
#include "net.h"

#define DUMMY_MTU UINT16_MAX /* maximum size of IP datagram */

#define DUMMY_IRQ INTR_IRQ_BASE // ダミーデバイスが使うirq number
/*
    dummy deviceの使用
    INPUT: なし．データを受信しない
    OUTPUT: 破棄．packet drop
*/
static int
dummy_transmit(struct net_device *dev, uint16_t type, const uint8_t *data, size_t len, const void *dst)
{
    debugf("dev=%s, type=ox%04x, le=%zu", dev->name, type, len);
    debugdump(data, len);
    // packet drop
    intr_raise_irq(DUMMY_IRQ); //テスト用に割り込みを発生させる
    return 0;
}

static int
dummy_isr(unsigned int irq, void *id)
{
    debugf("irq=%u, dev=%s", irq, ((struct net_device*)id)->name); // 呼び出されたことを出力する
    return 0;
}

static struct net_device_ops dummy_ops = {
    .transmit = dummy_transmit, // 現在，transmitのみを設定
};

struct net_device *
dummy_init(void)
{
    struct net_device *dev;

    dev = net_device_alloc(); // devを生成
    if (!dev) {
        errorf("net_device_alloc() failure");
        return NULL;
    }
    dev->type = NET_DEVICE_TYPE_DUMMY;
    dev->mtu = DUMMY_MTU;
    dev->hlen = 0; // 現状，headerなし
    dev->alen = 0; // 現状，addressなし
    dev->ops = &dummy_ops; // device driverが実装している関数のアドレスを保持する構造体へのポインタを設定
    if (net_device_register(dev) == -1) { // deviceの登録
        errorf("net_device_register() failure");
        return NULL;
    }
    intr_request_irq(DUMMY_IRQ, dummy_isr, INTR_IRQ_SHARED, dev->name, dev); // 割り込みハンドラdummy_isrを登録
    debugf("intialized device, dev=%s", dev->name);
    return dev;
}