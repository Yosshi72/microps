#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>

#include "platform.h"

#include "util.h"
#include "net.h"
#include "ip.h"
#include "icmp.h"

struct ip_hdr { // IP header
    uint8_t vhl; // versionとheader長合わせて8bitで扱う
    uint8_t tos;
    uint16_t total;
    uint16_t id;
    uint16_t offset; // flagとfragment_offsetを合わせて16bitで扱う
    uint8_t ttl;
    uint8_t protocol;
    uint16_t sum;
    ip_addr_t src; // ip address：ip_addr_tを使う
    ip_addr_t dst;
    uint8_t options[]; // 可変長なのでflexible_array_memberとする
};

struct ip_protocol { // ipの上位プロトコルを管理する構造体
    struct ip_protocol *next;
    uint8_t type;
    void (*handler)(const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, struct ip_iface *iface);
};

const ip_addr_t IP_ADDR_ANY       = 0x00000000; /* 0.0.0.0 */
const ip_addr_t IP_ADDR_BROADCAST = 0xffffffff; /* 255.255.255.255 */

static struct ip_iface *ifaces;
static struct ip_protocol *protocols; // registered protocol list

int
ip_addr_pton(const char *p, ip_addr_t *n) // ip addressをstr->binary(ip_addr_t)と変換
{
    char *sp, *ep;
    int idx;
    long ret;

    sp = (char *)p;
    for (idx = 0; idx < 4; idx++) {
        ret = strtol(sp, &ep, 10);
        if (ret < 0 || ret > 255) {
            return -1;
        }
        if (ep == sp) {
            return -1;
        }
        if ((idx == 3 && *ep != '\0') || (idx != 3 && *ep != '.')) {
            return -1;
        }
        ((uint8_t *)n)[idx] = ret;
        sp = ep + 1;
    }
    return 0;
}

char *
ip_addr_ntop(ip_addr_t n, char *p, size_t size) // ip addressをbinary(ip_addr_t)->strと変換
{
    uint8_t *u8;

    u8 = (uint8_t *)&n;
    snprintf(p, size, "%d.%d.%d.%d", u8[0], u8[1], u8[2], u8[3]);
    return p;
}

static void
ip_dump(const uint8_t *data, size_t len) // debug出力をする
{
    struct ip_hdr *hdr;
    uint8_t v, hl, hlen;
    uint16_t total, offset;
    char addr[IP_ADDR_STR_LEN];

    flockfile(stderr);
    hdr = (struct ip_hdr*)data;
    v = (hdr->vhl & 0xf0) >>4; // versionはvhlの上位4bit
    hl = (hdr->vhl & 0x0f); // header_lenはvhlの下位4bit
    hlen = hl <<2; // header_lenを32bit単位から8bit単位に変換
    fprintf(stderr, "       vhl: 0x%02x [v:%u, hl: %u (%u)]\n", hdr->vhl, v, hl, hlen);
    fprintf(stderr, "       tos: 0x%02x\n", hdr->tos);

    total = ntoh16(hdr->total);
    fprintf(stderr, "     total: %u (payload: %u)\n", total, total - hlen); // payloadの長さ：total_len - header_len
    fprintf(stderr, "        id: %u\n", hdr->id);
    
    offset = ntoh16(hdr->offset);
    fprintf(stderr, "    offset: 0x%04x [flags=%x, offset=%u]\n", offset, (offset & 0xe00) >>13, offset & 0x1fff); // offset：上位3bit, flags：下位13bit
    fprintf(stderr, "       ttl: %u\n", hdr->ttl);
    fprintf(stderr, "   protocol: %u\n", hdr->protocol);
    fprintf(stderr, "       sum: 0x%04x\n", hdr->sum);
    fprintf(stderr, "       src: %s\n", ip_addr_ntop(hdr->src, addr, sizeof(addr))); // ip addressをbinary->str
    fprintf(stderr, "       dst: %s\n", ip_addr_ntop(hdr->dst, addr, sizeof(addr)));
#ifdef HEXDUMP
    hexdump(stderr, data, len);
#endif
    funlockfile(stderr);
}

struct ip_iface *
ip_iface_alloc(const char *unicast, const char *netmask)
{
    struct ip_iface *iface;

    iface = memory_alloc(sizeof(*iface));
    if (!iface) {
        errorf("memory_alloc() failure");
        return NULL;
    }
    NET_IFACE(iface)->family = NET_IFACE_FAMILY_IP; // interface_familyを設定

    if (ip_addr_pton(unicast, &iface->unicast) != 0) { // unicastの情報をinterfaceに設定
        errorf("unicast error: unicast=%s", unicast);
        memory_free(iface);
        return NULL;
    }
    if (ip_addr_pton(netmask, &iface->netmask) != 0) { // netmaskの情報をinterfaceに設定
        errorf("netmask error: netmask=%s", netmask);
        memory_free(iface);
        return NULL;
    }
    iface->broadcast = (iface->unicast & iface->netmask) | ~iface->netmask;
    return iface;
}

/* NOTE: must not be call after net_run() */
int
ip_iface_register(struct net_device *dev, struct ip_iface *iface) // ip interfaceの登録
{
    // ip interfaceのunicast, netmask, broadcastをchar型にcastする用
    char addr1[IP_ADDR_STR_LEN]; 
    char addr2[IP_ADDR_STR_LEN];
    char addr3[IP_ADDR_STR_LEN];

    // devにip interfaceを登録
    if (net_device_add_iface(dev, NET_IFACE(iface)) == -1) {
        errorf("net_device_add_iface() failure");
        return -1;
    }
    iface->next = ifaces; // ip interface listの先頭にifaceを挿入
    ifaces = iface;

    infof("registered: dev=%s, unicast=%s, netmask=%s, broadcast=%s", dev->name, 
        ip_addr_ntop(iface->unicast, addr1, sizeof(addr1)), 
        ip_addr_ntop(iface->netmask, addr2, sizeof(addr2)), 
        ip_addr_ntop(iface->broadcast,addr3, sizeof(addr3)));
    return 0;
}  

struct ip_iface *
ip_iface_select(ip_addr_t addr) // ip interfaceの検索
{
    struct ip_iface *iface;
    for (iface = ifaces; iface; iface = iface->next) {
        if (iface->unicast == addr) { // 引数で指定されたIP addressを持つinterfaceを返す
            return iface;
        }
    }
    return NULL;
}

int // protocl登録
ip_protocol_register(uint8_t type, void (*handler)(const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, struct ip_iface *iface))
{
    struct ip_protocol *entry;

    // 重複登録の確認
    for (entry=protocols; entry; entry = entry->next) {
        if (type == entry->type) { // protocolが重複したらエラー
            errorf("already registered, type=0x%u", type);
            return -1;
        }
    } 

    // protocolの登録
    entry = memory_alloc(sizeof(*entry));
    if (!entry) {
        errorf("memory_alloc() failure");
        return -1;
    }
    entry->type = type;
    entry->handler = handler;
    entry->next = protocols; // ip_protocol listの更新
    protocols = entry;
    infof("registered, type=%u", type);
    return 0;
}

static void
ip_input(const uint8_t *data, size_t len, struct net_device *dev) // ip_headerの検証
{
    struct ip_hdr *hdr;
    uint8_t v;
    uint16_t hlen, total, offset;
    struct ip_iface *iface;
    char addr[IP_ADDR_STR_LEN];

    if (len < IP_HDR_SIZE_MIN) { // input_dataの長さがip headerの最小サイズより小さい
        errorf("too short");
        return;
    }
    hdr = (struct ip_hdr*) data; // input_dataをip_hder構造体のpointerにcast

    v = (hdr->vhl & 0xf0) >>4;
    if (v != IP_VERSION_IPV4) { // versionがIPv4と一致しない
        errorf("ip version error: v=%u", v);
        return;
    }

    hlen = (hdr->vhl & 0x0f) <<2;
    if (len < hlen) { // input_dataの長さがheader_lenより小さい
        errorf("header length error: len=%zu < hlen=%u", len, hlen);
        return;
    }

    total = ntoh16(hdr->total);
    if (len < total) { // input_dataの長さがtotal_lenより小さい
        errorf("total length error; len=%zu < total=%u", len, total);
        return;
    }

    if (cksum16((uint16_t *)hdr, hlen, 0) != 0) { // checksumの検証
        errorf("checksum error: sum=0x%04x, verify = 0x%04x", ntoh16(hdr->sum), ntoh16(cksum16((uint16_t *)hdr, hlen, -hdr->sum)));
        return;
    }
    offset = ntoh16(hdr->offset); // fragmentがあるかをチェック
    if (offset & 0x2000 || offset & 0x1fff) { // MF bitが立っている or fragment_offsetに値がある
        errorf("fragments does not support");
        return;
    }

    // filter IP datagram
    iface = (struct ip_iface *)net_device_get_iface(dev, NET_IFACE_FAMILY_IP);
    if (!iface) { // iface is not registered to dev
        return;
    }
    /*
        dstの検証：interfaceのunicast, subnetのbroadcast, broadcastに一致しなければ，
        他ホスト宛と判断して中断
    */
    if (hdr->dst != iface->unicast && hdr->dst != IP_ADDR_BROADCAST && hdr->dst != iface->broadcast) {
        return;
    }
    debugf("dev=%s, iface=%s, protocol=%u, total=%u", 
        dev->name, ip_addr_ntop(iface->unicast, addr, sizeof(addr)), hdr->protocol, total); // 入力関数が呼び出されたことだけわかればいい
    ip_dump(data, total);
    // protocolの検索し，データを振り分ける
    struct ip_protocol *proto;
    for (proto = protocols; proto; proto = proto->next) {
        if (hdr->protocol == proto->type) {
            proto->handler((uint8_t *)hdr + hlen, total - hlen, hdr->src, hdr->dst, iface);
            return;
        }
    }
    /* unsupported protocol */
}

static int // devから送信
ip_output_device(struct ip_iface *iface, const uint8_t *data, size_t len, ip_addr_t dst)
{
    uint8_t hwaddr[NET_DEVICE_ADDR_LEN] = {};

    // ARPによるアドレス解決が必要か
    if (NET_IFACE(iface)->dev->flags & NET_DEVICE_FLAG_NEED_ARP) {
        if (dst == iface->broadcast || dst == IP_ADDR_BROADCAST) { // dstがbroadcastならarpによるアドレス解決をせずにdevのbroadcast ip addressを使う
            memcpy(hwaddr, NET_IFACE(iface)->dev->broadcast, NET_IFACE(iface)->dev->alen);
        } else { // TODO: arp未実装のためエラー
            errorf("arp does not implement");
            return -1;
        }
    }
    return net_device_output(NET_IFACE(iface)->dev, NET_PROTOCOL_TYPE_IP, data, len, hwaddr);
}

static ssize_t // ip datagramを生成する関数
ip_output_core(struct ip_iface *iface, uint8_t protocol, const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, uint16_t id, uint16_t offset)
{
    uint8_t buf[IP_TOTAL_SIZE_MAX];
    struct ip_hdr *hdr;
    uint16_t hlen;
    char addr[IP_ADDR_STR_LEN];

    hdr = (struct ip_hdr *)buf;

    // ip datagramを生成
    hlen = IP_HDR_SIZE_MIN;
    hdr-> vhl = (IP_VERSION_IPV4 <<4) | (hlen >> 2);
    hdr->tos = 0;
    hdr->total = hton16(hlen + len);
    hdr->id = hton16(id);
    hdr->offset = hton16(offset);
    hdr->ttl = 0xff;
    hdr->protocol = protocol;
    hdr->sum = 0; // 計算前にchecksum fieldを0にする
    hdr->src = src;
    hdr->dst = dst;
    hdr->sum = cksum16((uint16_t *)hdr, hlen, 0); // hdrの全フィールド埋めてから計算
    memcpy(hdr+1, data, len); // ip headerの直後にdataをコピー

    debugf("dev=%s, dst=%s, protocol=%u, len=%u",
        NET_IFACE(iface)->dev->name, ip_addr_ntop(dst, addr, sizeof(addr)), protocol, hlen + len);
    ip_dump(buf, hlen + len);
    return ip_output_device(iface, buf, hlen + len, dst); //生成したdatagramを実際にdevから送信するための関数に渡す
}

static uint16_t
ip_generate_id(void)
{
    static mutex_t mutex = MUTEX_INITIALIZER;
    static uint16_t id = 128;
    uint16_t ret;

    mutex_lock(&mutex);
    ret = id++;
    mutex_unlock(&mutex);
    return ret;
}

ssize_t
ip_output(uint8_t protocol, const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst)
{
    struct ip_iface *iface;
    char addr[IP_ADDR_STR_LEN];
    uint16_t id;
    // ip interfaceの検索
    if (src == IP_ADDR_ANY) { // routing未実装のため，送信元アドレスが指定されていない場合はerror
        errorf("ip routing does not implement");
        return -1;
    } else {
        iface = ip_iface_select(src);
        if (!iface) {
            errorf("iface not found, src=%s", ip_addr_ntop(src, addr, sizeof(addr)));
            return -1;
        }
    }
    /*
        dstへ到達可能かチェック
        dstがinterfaceのnetwork address範囲内 or broadcast addressなら到達可能
    */
    if ((dst & iface->netmask) != (iface->unicast & iface->netmask)) {
        if (dst != IP_ADDR_BROADCAST) {
            errorf("unreachable to dst(addr=%s)", ip_addr_ntop(src, addr, sizeof(addr)));
            return -1;
        }
    }
    // fragmentationをサポートしないのでMTUを超えるならエラー
    if (NET_IFACE(iface)->dev->mtu < IP_HDR_SIZE_MAX + len) {
        errorf("too long, dev=%s, mtu=%u < %zu",
            NET_IFACE(iface)->dev->name, NET_IFACE(iface)->dev->mtu, IP_HDR_SIZE_MAX + len);
        return -1;
    }
    id = ip_generate_id(); // IP datagramのidをprotect
    // ip datagramを生成，出力するための関数を呼び出し
    if (ip_output_core(iface, protocol, data, len, iface->unicast, dst, id, 0) == -1) {
        errorf("ip_output_core() failure");
        return -1;
    }
    return len;
}

int
ip_init(void) // ipの初期化関数：IPのtypeを登録
{
    if (net_protocol_register(NET_PROTOCOL_TYPE_IP, ip_input) == -1) {
        errorf("net_protocol_register() failure");
        return -1;
    }
    return 0;
}