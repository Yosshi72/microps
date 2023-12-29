#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#include "util.h"
#include "net.h"
#include "ip.h"

struct net_protocol {
    struct net_protocol *next; // pointer to next protocol
    uint16_t type; // protocol type
    struct queue_head queue; /* input queue */
    void (*handler)(const uint8_t *data, size_t len, struct net_device *dev);
};

struct net_protocol_queue_entry {
    struct net_device *dev;
    size_t len;
    uint8_t data[];
};

/* NOTE: if you want to add/delete the entries after net_run(), you need to protect these lists with a mutex. */
static struct net_device *devices; // registered device list
static struct net_protocol *protocols; // registered protocol list

struct net_device *
net_device_alloc(void) // deviceの生成
{
    struct net_device *dev;

    dev = memory_alloc(sizeof(*dev)); // device構造体のメモリ領域を確保する
    if (!dev) {
        errorf("memory_alloc() failed");
        return NULL;
    }
    return dev;
}

/* NOTE: must not be call after net_run() */
int
net_device_register(struct net_device *dev) // deviceの登録
{
    static unsigned int index = 0;

    dev ->index = index++; // deviceのindexを指定
    snprintf(dev->name, sizeof(dev->name),"net%d", dev->index); // device nameを生成：netX, Xはindex
    // 生成したdevをdevice listの先頭に追加する
    dev->next = devices;
    devices = dev;
    infof("registered, dev=%s, type=0x%04x", dev->name, dev->type);
    return 0;
}

static int
net_device_open(struct net_device *dev) // deviceのopen
{
    if (NET_DEVICE_IS_UP(dev)) { // 既にdeviceがupしていたらerrorを返す
        errorf("already opened, dev=%s", dev->name);
        return -1;
    }
    if (dev->ops->open) { // device driverのopen関数を呼び出し
        if (dev->ops->open(dev) == -1) {
            errorf("failure, dev=%s", dev->name);
            return -1;
        }
    }
    dev->flags |= NET_DEVICE_FLAG_UP;
    infof("dev=%s, state=%s", dev->name, NET_DEVICE_STATE(dev));
    return 0;
}

static int
net_device_close(struct net_device *dev) // deviceのclose
{
    if (!NET_DEVICE_IS_UP(dev)){ // deviceが既にupじゃなかったらerror
        errorf("not opened, dev=%s", dev->name);
        return -1;
    }
    if (dev->ops->close) { // device driverのclose関数を呼び出し
        if (dev->ops->close(dev) == -1) {
            errorf("failure, dev=%s", dev->name);
            return -1;
        }
    }
    dev->flags &= ~NET_DEVICE_FLAG_UP; // flagsをdownにする
    infof("dev=%s, state=%s", dev->name, NET_DEVICE_STATE(dev));
    return 0;
}

/* NOTE: must not be call after net_run() */
int // ip interfaceをdevに登録
net_device_add_iface(struct net_device *dev, struct net_iface *iface)
{
    struct net_iface *entry;

    for (entry = dev->ifaces; entry; entry = entry->next) {
        if (entry->family == iface->family) {
            errorf("already exists, dev=%s, family=%d", dev->name, entry->family);
            return -1;
        }
    }
    iface->dev = dev;
    iface->next = dev->ifaces;
    dev->ifaces = iface; // devのinterface listの先頭にifaceを挿入 
    
    return 0;
}

struct net_iface *
net_device_get_iface(struct net_device *dev, int family) // devに紐づくinterfaceを検索
{
    struct net_iface *iface;
    for (iface = dev->ifaces; iface; iface = iface->next) { // devのinterface_listを巡回
        if (iface->family == family) { // familyが一致するinterfaceを返す
            return iface;
        }
    } 
    return NULL; // 見つからなかったらNULLを返す

}

int
net_device_output(struct net_device *dev, uint16_t type, const uint8_t *data, size_t len, const void *dst) // deviceへの出力
{   
    // device stateを確認
    if (!NET_DEVICE_IS_UP(dev)) {
        errorf("not opened, dev=%s", dev->name);
        return -1;
    }
    // data sizeを確認
    if (len > dev->mtu) {
        errorf("too long, dev=%s, mtu=%u", dev->name, type, len);
        return -1;
    }
    debugf("dev=%s, type=-x%04x, len=%zu", dev->name, dev->type, len);
    debugdump(data, len);
    if (dev->ops->transmit(dev, type, data, len, dst) == -1) {
        errorf("device transmit failire, dev=%s, len=%zu", dev->name, len);
        return -1;
    }
    return 0;
}

int
net_protocol_register(uint16_t type, void (*handler)(const uint8_t *data, size_t len, struct net_device *dev))
{
    struct net_protocol *proto;

    for (proto=protocols; proto; proto = proto->next) {
        if (type == proto->type) { // protocolが重複したらエラー
            errorf("already registered, type=0x%04x", type);
            return -1;
        }
    }
    proto = memory_alloc(sizeof(*proto)); // protocol構造体のメモリを確保
    if (!proto) {
        errorf("memory_alloc() failure");
        return -1;
    }
    proto->type = type;
    proto->handler = handler;
    proto->next = protocols; // protocol listの更新
    protocols = proto;
    infof("registered, type=0x%04x", type);
    return 0;
}

int
net_input_handler(uint16_t type, const uint8_t *data, size_t len, struct net_device *dev) // deviceからの入力を処理．deviceが受信したパケットをprotocol stackに渡す
{
    struct net_protocol *proto;
    struct net_protocol_queue_entry *entry;

    for (proto = protocols; proto; proto = proto->next) {
        if (proto->type == type) { // 受信したパケットのprotocolがprotocol listに含まれていたら，
            entry = memory_alloc(sizeof(*entry) + len); // entry構造体とdata sizeの分だけメモリ確保
            if (!entry) {
                errorf("memory_alloc() failure");
                return -1;
            }
            entry->dev = dev; // 新しいentryにmetadataの設定
            entry->len = len;
            memcpy(entry->data, data, len); // 受信データをentryにコピー
            queue_push(&proto->queue, entry); // queueにentryを追加

            debugf("queue pushed (num:%u), dev=%s, type=0x%04x, len=%zu",
                proto->queue.num, dev->name, type, len);
            debugdump(data, len);
            intr_raise_irq(INTR_IRQ_SOFTIRQ); // protocolの受信queueにエントリを追加した後，software interruptionを発生させる
            return 0;
        }
    }
    /* unsupported protocol */ //サポートされていないプロトコルのパケットは捨てる
    return 0;
}

int
net_softirq_handler(void) // software interruption発生時に呼び出される
{
    struct net_protocol *proto;
    struct net_protocol_queue_entry *entry;

    for (proto = protocols; proto; proto = proto->next) { // supportしているすべてのプロトコルを確認
        while (1) {
            entry = queue_pop(&proto->queue); // 受信queueからentryを取り出す
            if (!entry) {
                break;
            }
            debugf("queue popped (num:%u), dev=%s, type=0x%04x, len=%zu", proto->queue.num, entry->dev->name, proto->type, entry->len);
            debugdump(entry->data, entry->len);
            proto->handler(entry->data, entry->len, entry->dev); // protocolの入力関数を呼び出し
            memory_free(entry);
        }
    }
    return 0;
}

int
net_run(void) // protocol stackの起動
{
    struct net_device *dev;

    if (intr_run() == -1) { // 割り込み機構の起動
        errorf("intr_run() failure");
        return -1;
    }
    debugf("open all devices...");
    for (dev = devices; dev; dev = dev->next) { // 登録済みデバイスを全てopen
        net_device_open(dev);
    }
    debugf("running");
    return 0;
}

void
net_shutdown(void) // protocol stackの停止
{
    struct net_device *dev;
    intr_shutdown(); // 割り込み機構の終了
    debugf("close all devices...");
    for (dev= devices; dev; dev = dev->next) { // 登録済みデバイスを全てclose
        net_device_close(dev);
    }
    debugf("shutting down");
}

int
net_init(void)
{
    if (intr_init() == -1) { // 割り込み機構の初期化
        errorf("intr_init() failure");
        return -1;
    }
    if (ip_init() == -1) { // protocol stack初期化時にipの初期化関数を呼び出し
        errorf("ip_init() failure");
        return -1;
    }
    infof("intitialized");
    return 0;
}