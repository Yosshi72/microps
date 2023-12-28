#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

#include "platform.h"

#include "util.h"

struct irq_entry {
    struct irq_entry *next; // pointer to next irq
    unsigned int irq; // 割り込み番号
    int (*handler)(unsigned int irq, void *dev); // 割り込みハンドラ
    int flags;
    char name[16]; // debug用の名前
    void *dev; // 割り込み発生源のデバイス
};

/* NOTE: if you want to add/delete the entries after intr_run(), you need to protect these lists with a mutex. */
static struct irq_entry *irqs; // IRQ listの先頭を指すポインタ

static sigset_t sigmask;
 
static pthread_t tid; // 割り込みスレッドのthreadID
static pthread_barrier_t barrier; // thread間の同期のためのbarrier

int
intr_request_irq(unsigned int irq, int (*handler)(unsigned int irq, void *dev), int flags, const char *name, void *dev)
{
    struct irq_entry *entry;

    debugf("irq=%u, flags=%d, name=%s", irq, flags, name);
    for (entry=irqs; entry; entry = entry->next) {
        if (entry->irq == irq) { // irq番号が既に登録されている場合，
            if (entry->flags ^ INTR_IRQ_SHARED || flags ^ INTR_IRQ_SHARED) { // IRQ番号の共有が許可されているかをチェック
                errorf("conflict with already registered IRQs");
                return -1;
            }
        }
    }
    // irq listに新しいentryを追加
    entry = memory_alloc(sizeof(*entry));
    if (!entry) {
        errorf("memory_alloc() failure");
        return -1;
    }
    entry->irq = irq;
    entry->handler = handler;
    entry->flags = flags;
    strncpy(entry->name, name, sizeof(entry->name)-1);
    entry->dev = dev;
    entry->next = irqs;
    irqs = entry;
    sigaddset(&sigmask, irq);
    debugf("registered: irq=%u, name=%s", irq, name);
    return 0;
}

int
intr_raise_irq(unsigned int irq)
{
    return pthread_kill(tid, (int)irq); // 割り込みスレッドへシグナルを送信
}

static void *
intr_thread(void *arg) // 割り込みスレッドのエントリポイント
{
    int terminate =0, sig, err;
    struct irq_entry *entry;

    debugf("start...");
    pthread_barrier_wait(&barrier); // main threadと同期
    while (!terminate) {
        err = sigwait(&sigmask, &sig); // 割り込みに見立てたsignalが発生するまで待機
        if (err) {
            errorf("sigwait() %s", strerror(err));
            break;
        }
        switch (sig) { // 発生したシグナルに応じた処理を実行
            case SIGHUP: // SIGHUP：割り込みスレッドへ終了を通知するためのシグナル
                terminate = 1; 
                break;
            default: // デバイス割り込み用のシグナルが発生した
                for (entry = irqs; entry; entry = entry->next) { // irqを巡回
                    if (entry->irq == (unsigned int)sig) { // irq番号が一致するエントリの割り込みハンドラを呼び出し
                        debugf("irq=%d, name=%s", entry->irq, entry->name);
                        entry->handler(entry->irq, entry->dev);
                    }
                }
                break;
        }
    }
    debugf("terminated");
    return NULL;
}

int
intr_run(void)
{
    int err;

    err = pthread_sigmask(SIG_BLOCK, &sigmask, NULL); // signal maskの設定
    if (err) {
        errorf("pthread_sigmask() %s", strerror(err));
        return -1;
    }
    err = pthread_create(&tid, NULL, intr_thread, NULL); // 割り込みスレッドの起動
    if (err) {
        errorf("pthread_create() %s", strerror(err));
        return -1;
    }
    pthread_barrier_wait(&barrier); // スレッドが動き出すまで待つ
    return 0;
}

void
intr_shutdown(void)
{
    if (pthread_equal(tid, pthread_self()) != 0) { // 割り込みスレッドが起動済みかどうか
        // Thread not created
        return;
    }
    pthread_kill(tid, SIGHUP); // 割り込みスレッドにSIGHUPを送信
    pthread_join(tid, NULL); // 割り込みスレッドが完全に終了するまで待つ
}

int
intr_init(void)
{
    tid = pthread_self(); // スレッドIDの初期値にメインスレッドのIDを設定
    pthread_barrier_init(&barrier, NULL, 2); // pthread_barrierの初期化
    sigemptyset(&sigmask); // シグナル集合を空にして初期化
    sigaddset(&sigmask, SIGHUP); // シグナル集合にSIGHUPを追加
    return 0;
}