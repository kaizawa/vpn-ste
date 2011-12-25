/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 1986, 2010, Oracle and/or its affiliates. All rights reserved.
 */

/*
 * Copright (c) 2004-2010  Kazuyoshi Aizawa <admin2@whiteboard.ne.jp>
 * All rights reserved.
 */
/********************************************************************
 *  ste.c
 *  
 *  仮想 NIC（ネットワークインターフェースカード）のデバイスドライバ。
 *  
 *  IP モジュールから受け取ったデータを Ethernet フレームとしてユーザ
 *  プロセスである sted に渡し、また、sted から受け取った Ethernet
 *  フレームのデータ部を IP モジュールに渡す。
 *
 *  本ドライバの特徴:
 *   o カーネルモジュールである
 *   o 実在の NIC のように ifconfig で設定が可能。
 *   o GLD ベースのドライバでは無い。
 *   o DLPI V2 Style 2
 *   o クローンデバイス
 *  
 *  cc -c -xarch=v9 -O -DSOL10 -D_KERNEL -D_SYSCALL32 ste.c ste.h
 *  ld -dn -r ste.o -o /kernel/drv/ste
 *
 *  変更履歴
 *      sted のオープンした Stream head の hiwater mark を大きく
 *      するように変更した。(STR_HIWAT にて値を設定。デフォルトは 20,000 Byte)
 *   2004/12/29
 *     o IP,ARP に渡す前に、あて先イーサネットアドレスを確認し、自分宛て、
 *       ブロードキャスト、マルチキャスト宛てでないものは破棄するようにした。
 *     o ste_soft 構造体内の Ethernet アドレスについて ether_addr 構造体
 *       を利用するように変更した。
 *   2005/01/03
 *     o DLPI のステータスを管理するように変更した。
 *       これとあわせて、ste_dl_detach_req() と、ste_dl_unbind_req() 関数を追加した。
 *   2005/01/04
 *     o open(9E) で新しく割り当てるマイナーデバイス番号を、使われていない
 *       マイナーデバイス番号の最小値を割り当てるように変更した。
 *   2005/01/07
 *     o ste ドライバを D_MTQPAIR に変更。
 *     o write サイドのメッセージを全て、一旦 putq(9F) にて queue に入れるようにした。
 *     o RAW モードの stream からのメッセージの Ethernet アドレスを確認し、
 *       必要であれば、IP や ARP にもコピーしたメッセージを渡すようにした。
 *   2005/01/09
 *     o ste_send_up() で、sted の stream へメッセージを配信しないようにした。
 *   2005/01/12
 *     o 不要な global lock を取得しないようにした。
 *     o DL_ENABMULTI_REQ に肯定応答するようにした。（実際にはまだ、何もしない）
 *       これにより、arp -a の multicast の ethernet address が正しく表示されるようになり、
 *       また、ifconfig -a でも MULTI_BCAST フラグが表示されなくなった。 
 *     o DL_BIND_REQ, DL_INFO_REQ で回答する DLSAP アドレスを正しく EtherAddr + SAP にした。
 *       これにより、arp -a にて Ethernet address の下位 bit が表示されない問題が解消した。
 *   2005/01/19
 *     o _init(), _fini() の error 処理の修正を行った。
 *   2005/08/08
 *     o Solaris 10 で /etc/hostname.ste0 を作成してから reboot すると PANIC する問題
 *       を修正。_fini() にて mod_remove() 前に ddi_soft_state_fini() を呼んでいたのが原因。
 *     o Solaris 10 で、インスタンスが利用中にもかかわらず modunload(1M) を使われると
 *       detach(9F) ルーチンが呼ばれてしまい、その後のパケット処理にて PANIC してしまう
 *       問題を修正。どちらも Solaris 9 では発生しない・・。
 *   2005/08/10
 *     o Debug 出力用のマクロを変更し、引数の数に関係なく、単一のマクロを使うようにした。
 *   2005/08/14
 *     o ste_str 構造体毎に lock を配置し、ロックの有効範囲を狭めた。
 *     o グローバルロック(ste_global_lock)を取得するコード上からなくした。
 *     o ロックを長時間保持する必要があった処理では、ロックを保持するのではなく構造体の
 *       参照カウントを増やすように変更し、これによってストリームの存在を保障するようにした。
 *     o いままでのグローバルな ste_str 構造体のリストの他に、インスタンス毎のリストを作った。
 *       これにより、インスタンスに関係の必要のない ste_str の検査が不要になった。
 *    2006/03/25
 *      o SOL10 が define していた場合に DL_ATTACH, DL_DETACH, open(9F) に qassociate(9F)
 *        が追加されるように変更した。これは Solaris 10 から導入されたもので、DDI-compliant
 *        のために必要。
 *      o 2005/8/14 のコード変更による不具合を解消
 *    2006/04/09
 *      o x86 Solaris にて ether_header 構造体の ether_type 値と ste_str 構造体の sap 値の
 *        コピー時にバイトオーダーの違いを考慮せず、不適切なコピーが行われていたのを修正した。
 *        これにより x86 Solaris 上でも動作可能なことを確認した。
 *    2006/05/13
 *      o MAC アドレスを現在時と、LBOLT から動的に生成するように変更した。
 *      o ste_unitdata_ind() で、src と dest のアドレスを ether_addr 構造体から stedladdr_t 構造体
 *        に変更した。
 *      o ste_stream_is_eligible() を変更し、受信フレームが 802.3 フレームであった場合には SAP 値 0　
 *      　のストリームへの配送を許可するようにした。
 *    2006/05/30
 *      o sted がオープンしたストリームかどうかをチェックするための STE_SVCQ フラグを追加した。
 *    2006/06/02
 *      o ste_dl_unitdata_req() 内で SAP 値の取得方法を変更した。
 *    2006/06/05
 *      o _fini() のエラー処理を変更した。
 *      o 物理 NIC の様に、Link up/Link dow メッセージを出力するようにした。
 ********************************************************************/

#include <netinet/in.h>
#include <inet/common.h>
#include <inet/ip.h>
#include <sys/signal.h>
#include <sys/errno.h>
#include <sys/cred.h>
#include <sys/stat.h>
#include <sys/conf.h>
#include <sys/kmem.h>
#include <sys/dlpi.h>
#include <sys/ethernet.h>
#include <sys/modctl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stream.h>
#include <stropts.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/devops.h>
#include <sys/strsun.h>
#include "ste.h"

#define STR_HIWAT 20000
#define DEVNAME "ste"
#define MAX_MSG 256  // SYSLOG に出力するメッセージの最大文字数
#define STESTR_HOLD(strp) \
        mutex_enter(&(strp->lock));\
        strp->refcnt++;\
        mutex_exit(&(strp->lock));
#define STESTR_RELEASE(strp) \
        mutex_enter(&(strp->lock));\
        strp->refcnt--;\
        cv_broadcast(&(strp->cv));\
        mutex_exit(&(strp->lock));

/*
 * DEBUG を define すると、DEBUG モード ON になる 
 * #define DEBUG
 */
static int minor_count = 0;
static void *ste_soft_root = NULL;
static unsigned char broadcastaddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/*
 * DEBUG 用の出力ルーチン
 * cmn_err(9F) にて syslog メッセージを出力する。
 * DEBUG を on にすると、かなりのデバッグメッセージが記録される。
 */
#ifdef DEBUG
#define  DEBUG_PRINT(args)  debug_print args
#else
#define DEBUG_PRINT
#endif

typedef struct ste_soft ste_soft_t;
typedef struct ste_str ste_str_t;

/*
 * stream 毎(ste オープン毎）に作成される構造体
 */
struct ste_str 
{
    kmutex_t     lock;            // この構造体のデータを保護するロック
    kcondvar_t	 cv;              // condition variable    
    ste_str_t   *inst_next;       // インスタンス毎のリストの次の ste_str 構造体へのポインタ 
    ste_str_t   *str_next;        // グローバルリストの次の ste_str 構造体へのポインタ
    int          refcnt;          // 参照カウント             
    queue_t     *qptr;            // read queue 
    ste_soft_t  *stesoft;         // 関連する ste デバイスのインスタンスの構造体    
    dev_t        minor;           // このストリームのマイナーデバイス番号
    t_uscalar_t  sap;             // SAP value
    int          flags;           // フラグ。プロミスキャスモードや、RAW モード等の情報を保持する。
    int          dl_state;        // この Stream の DLPI ステータス。sys/dlpi.h 参照
};

/* str_ste 内で使われるフラグ */
#define STE_PROMISCON  0x2  // プロミスキャスモード
#define STE_RAWMODE    0x4  // RAW モード
#define STE_SVCQ       0x8  // sted デーモンのオープンしたストリーム

/* 仮想 NIC のインスタンス（PPA）毎の構造体 */
struct ste_soft 
{
    kmutex_t           lock;           // この構造体のデータを保護するロック 
    dev_info_t        *devi;           // device infor 構造体 
    int                instance;       // NIC のインスタンス番号 
    queue_t           *svcq;           // sted の stream の read サイドの queue 
    struct ether_addr  etheraddr;      // このインスタンスの Ethernet アドレス 
    ste_str_t          str_list_head;  // このインスタンスにアタッチしている stream のリストのヘッド
    int                link_warning;   // Link down の警告回数
};

/* DLSAP アドレス */
typedef struct ste_dlsapaddr
{
    struct ether_addr  etheraddr;  // Ethernet アドレス
    ushort_t           sap;        // SAP 値
} stedladdr_t;

/* DLSAP アドレス長 */
#define STEDLADDRL ETHERADDRL + sizeof(ushort_t)

/*
 * stream 毎の構造体(ste_str)のグローバルリストのトップ。
 * グローバルリストとはインスタンスに無関係に、全ての
 * stream のリストということ。
 */
ste_str_t   ste_str_g_head;

/*
 * ste ドライバのグローバルロック。（初期化のみ。使っていない）
 */
kmutex_t     ste_global_lock;

static int ste_open (queue_t*, dev_t*, int, int, cred_t*);
static void ste_send_up(queue_t*, mblk_t*);
static int ste_wput (queue_t*, mblk_t*);
static int ste_wsrv (queue_t*);
static int ste_proto_wput(queue_t*, mblk_t*);
static int ste_ioctl_wput(queue_t*, mblk_t*);
static int ste_data_wput(queue_t *, mblk_t *);
static int ste_close (queue_t*, int, int, cred_t*);
static int ste_getinfo(dev_info_t *, ddi_info_cmd_t, void *, void **);
static int ste_identify(dev_info_t *);
static int ste_attach(dev_info_t *, ddi_attach_cmd_t);
static int ste_detach(dev_info_t *, ddi_detach_cmd_t);
static int ste_dl_info_req(queue_t *, mblk_t *);
static int ste_dl_unitdata_req(queue_t *, mblk_t *);
static int ste_dl_attach_req(queue_t *, mblk_t *);
static int ste_dl_detach_req(queue_t *, mblk_t *);
static int ste_dl_bind_req(queue_t *, mblk_t *);
static int ste_dl_unbind_req(queue_t *, mblk_t *);
static int ste_dl_phys_addr_req(queue_t *, mblk_t *);
static int ste_dl_set_phys_addr_req(queue_t *, mblk_t *);
static int ste_dl_promiscon_req(queue_t *, mblk_t *);
static int ste_dl_enabmulti_req(queue_t *, mblk_t *);
static void ste_send_dl_ok_ack(queue_t *, mblk_t *, t_uscalar_t );
static void ste_send_dl_error_ack(queue_t *, mblk_t *, t_uscalar_t, int, int);
static struct ste_str *ste_str_alloc(dev_t, queue_t*);
static int ste_str_free(dev_t);
static struct ste_str *ste_str_find_by_minor(dev_t);
static void debug_print(int , char *, ...);
static void ste_send_sted(queue_t *, mblk_t *);
static mblk_t *ste_create_dl_unitdata_ind(mblk_t *);
static int  ste_instance_is_busy(int);
static int  ste_stream_is_eligible(ste_str_t *, struct ether_header *, t_uscalar_t );
static void ste_add_to_inst_list(ste_str_t *, ste_str_t *);
static void ste_delete_from_inst_list(ste_str_t *, ste_str_t *);
static void ste_generate_mac_addr(struct ether_addr *);
static int  ste_msg_len(mblk_t *);

static struct module_info minfo =
{
    0xfade,   /* モジュール番号     */
    "ste",    /* モジュール名       */
    1,        /* 最小パケットサイズ */
    INFPSZ,   /* 最大パケットサイズ */
    50000,    /* high water mark */
    10000     /* low water mark  */
};

/*
 * read サイドのルーチン
 */
static struct qinit drv_rinit =
{
    NULL,        /* put(9E) ルーチン */
    NULL,        /* srv(9E) ルーチン */
    ste_open,    /* open ルーチン    */
    ste_close,   /* close ルーチン   */
    NULL,        /*  */                  
    &minfo,      /* module info 構造体 */
    NULL         /* module stat 構造体 */
};

/*
 * write サイドのルーチン
 */
static struct qinit drv_winit =
{
    ste_wput,    /* put(9E) ルーチン */
    ste_wsrv,    /* srv(9E) ルーチン */
    NULL,        /* open ルーチン    */
    NULL,        /* close ルーチン   */
    NULL,        /*  */                     
    &minfo,      /* module info 構造体 */
    NULL         /* module stat 構造体 */
};

/*
 * ste ドライバの streamtab 構造体
 */
struct streamtab ste_drv_info=
{
    &drv_rinit,   /* pointer to read size qinit */ 
    &drv_winit,   /* pointer to write size qinit */
    NULL,         /* for multiprexer */            
    NULL          /* for multiprexer */            
};


/*
 * cb_ops - character/block エントリーポイント構造体
 */
static struct cb_ops ste_cb_ops = {
    nodev,         /* cb_open     */
    nodev,         /* cb_close    */
    nodev,         /* cb_strategy */
    nodev,         /* cb_print    */
    nodev,         /* cb_dump     */
    nodev,         /* cb_read     */
    nodev,         /* cb_write    */
    nodev,         /* cb_ioctl    */
    nodev,         /* cb_devmap   */
    nodev,         /* cb_mmap     */
    nodev,         /* cb_segmap   */
    nochpoll,      /* cb_chpoll   */
    ddi_prop_op,   /* cb_prop_op  */
    &ste_drv_info, /* cb_stream   */
    (D_NEW |D_MP|D_MTQPAIR) /* cb_flag     */
};

/*
 * device operations 構造体
 */
static struct dev_ops ste_ops = {
    (DEVO_REV),               /* devo_rev      */
    (0),                      /* devo_refcnt   */
#ifdef SOL10
    (ddi_no_info),            /* stub for getinfo. introduced from Solaris 10 */   
#else    
    (ste_getinfo),            /* devo_getinfo  */
#endif    
    (nulldev),                /* devo_identify */
    (nulldev),                /* devo_probe    */
    (ste_attach),             /* devo_attach   */
    (ste_detach),             /* devo_detach   */
    (nodev),                  /* devo_reset    */
    &(ste_cb_ops),            /* devo_cb_ops   */
    (struct bus_ops *)(NULL)  /* devo_bus_ops  */        
};

/*
 * ローダブルモジュールのためのリンケージ構造体
 */
static struct modldrv modldrv = {
    &mod_driverops,          /* pointer to mod_driverops*/
    "virtual NIC driver"   , /* ドライバの説明          */
    &ste_ops                 /* driver ops              */
};

/*
 * module linkage リンケージ構造体
 * モジュールをロードする _init(9E) の中の mod_install によって使われる。
 * modlstrmod と modldrv へのポインタを含む
 */
static struct modlinkage modlinkage =
{
    MODREV_1,               /* revision of modules system */
    (void *)&modldrv,       /* driver linkage structure   */
    NULL                    /* NULL terminate             */
};

int
_init()
{
    int err;

    DEBUG_PRINT((CE_CONT,"_init called"));
    /*
     * デバイス管理構造体の管理用の ste_soft_root を初期化
     * ste のデバイス管理構造体は ste_soft_t として定義されている。
     */
    if (ddi_soft_state_init(&ste_soft_root, sizeof(ste_soft_t), 1) != 0) {
        return(DDI_FAILURE);
    }

    /*
     * ste_str_g_head の next のポインタを明示的に NULL にセット
     * _init が呼ばれた時点で、このアドレスにごみ（前回 driver がロードされたときの
     * ものとか) が入っているのを防ぐ。
     */
    ste_str_g_head.str_next = NULL;

    mutex_init(&ste_global_lock, NULL, MUTEX_DRIVER, NULL);
    
    err = mod_install(&modlinkage);
    if( err != 0){
          mutex_destroy(&ste_global_lock);
          ddi_soft_state_fini(&ste_soft_root);            
          cmn_err(CE_CONT,"_init: mod_install returned with error %d",err);
    }
    return(err);
}

int
_info(struct modinfo *modinfop)
{
    int err;

    DEBUG_PRINT((CE_CONT,"_info called"));
    err = mod_info(&modlinkage, modinfop);
    return(err);
}

int
_fini()
{
    int err;

    DEBUG_PRINT((CE_CONT,"_finit called"));
    err =  mod_remove(&modlinkage);
    if( err != 0){
          DEBUG_PRINT((CE_CONT,"_fini: mod_remove returned with error %d",err));
          return(err);
    }
    ddi_soft_state_fini(&ste_soft_root);
    mutex_destroy(&ste_global_lock);    
    return(err);
}

/*****************************************************************************
 * ste_open() !CHECKED!
 * 
 * ste の open(9E) ルーチン
 * ste は DLPI Version 2 Style 2 の DLS プロバイダー
 *****************************************************************************/
static int
ste_open(queue_t *q, dev_t *devp, int oflag, int sflag, cred_t *cred)
{ 
    ste_str_t *stestr; /* この stream の ste_str 構造体 */
    ste_str_t *strp;   /* 作業用のポインタ */
    minor_t newminor;

    DEBUG_PRINT((CE_CONT,"ste_open called"));
    
    if (sflag != CLONEOPEN){
        DEBUG_PRINT((CE_CONT,"ste_open: Not CLONEOPEN"));
        return(ENXIO);  /* Clone オープンでなくてはならない */
    }
    if (q->q_ptr){ /* すでにオープンされている!? */
        cmn_err(CE_CONT,"ste_open: q is already opend");
        return(EBUSY);
    }
    /*
     * 使われていない最小のマイナーデバイス番号を探す
     * このドライバは D_MTQPAIR なので ste_open() は排他的に呼ばれる
     * ので newminor がバッティングすることはないはず。
     */
    for ( newminor = 0 ; ; newminor++){
        if((strp = ste_str_find_by_minor(newminor)) == NULL)
            break;
        STESTR_RELEASE(strp);
    }
    
    if((stestr = ste_str_alloc(newminor, q)) == NULL){
        cmn_err(CE_CONT, "ste_open: ste_str_alloc failed");
        return(EAGAIN);
    }
    
    DEBUG_PRINT((CE_CONT, "ste_open: new minior = %d",newminor));

    /*
     * 新しいデバイス番号（メジャーとマイナーのペア）を作り, devp を
     * リセットする。getmajor() はメジャー番号を取得する
     */
    *devp = makedevice(getmajor(*devp), newminor);
    
    /* read サイド、write サイドともに、q_ptr は同じ構造体を指す */
    q->q_ptr = WR(q)->q_ptr = stestr;
    qprocson(q);
#ifdef SOL10
        /*
         * Solaris 10 で追加になったらしい。
         * See qassociate(9F) and ddi_create_minor_node(9f)
         */
     (void) qassociate(q, -1);
#endif     
    return(0);
}

/*****************************************************************************
 * ste_close()  !CHECKED!
 * 
 * ste の close(9E) ルーチン
 *****************************************************************************/
static int
ste_close(queue_t *q, int flag, int sflag, cred_t *cred)
{ 
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */

    DEBUG_PRINT((CE_CONT,"ste_close called"));
    
    qprocsoff(q);   /* put と service ルーチンを停止する */
    stestr = q->q_ptr;

    /*
     * この Stream がすでに PPA にアッタチしていたら、デタッチする。
     * もしこの Stream が仮想 NIC デーモンとして登録されていたら、登録解除する。
     */
    if((stesoft = stestr->stesoft) != NULL){
        if(stestr->flags & STE_SVCQ){
            mutex_enter(&(stesoft->lock));
            stesoft->svcq = NULL;
            mutex_exit(&(stesoft->lock));            
        }
        ste_delete_from_inst_list(&stesoft->str_list_head, stestr);
        stestr->stesoft = NULL;
        stestr->dl_state = DL_UNATTACHED;
    }
    
    DEBUG_PRINT((CE_CONT,"ste_close: closed minor = %d", stestr->minor));    

    if ( ste_str_free(stestr->minor) != 0){
        /* ste_str 構造体を開放できない。なので、close を完了できない*/
        cmn_err(CE_CONT,"ste_close: close failed");
        return(DDI_FAILURE);
    }

    /* read、write サイドの queue の q_ptr のリンクをはずす */
    q->q_ptr = WR(q)->q_ptr = NULL;
    return(0);
}

/*****************************************************************************
 * ste_send_up() !CHECKED!
 * 
 * ste の read サイドの put(9E) ルーチンに相当する。
 * メッセージを IP や promiscuous モードでオープンしているアプリケーションに
 * 伝送するために呼ばれる。
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 戻り値：
 *          無し
 *****************************************************************************/
static void
ste_send_up(queue_t *q, mblk_t *mp)
{ 
    ste_str_t *stestr;   // この stream の ste_str 構造体 
    ste_str_t *prevstrp; // 操作用の ste_str 構造体
    ste_str_t *strp;     // ste_str 構造体のリンクリストの処理用ポインタ 
    ste_soft_t *stesoft; // ste デバイスのインスタンスの構造体 
    struct ether_header *etherhdr; // メッセージの Ethernet ヘッダ
    t_uscalar_t sap;     // SAP 値
    t_uscalar_t ppa;     // NIC のインスタンス番号 
    mblk_t *dupmp;       // putmsg(9F) に渡すコピーされたメッセージ
    mblk_t *unitmp = NULL; // dl_unitdata_ind プリミティブのメッセージ
    mblk_t *tmpmp  = NULL; // 操作用のメッセージポインタ
#ifdef DEBUG
    unsigned char *rptr;
    size_t datalen = 0;
#endif    

    stestr = q->q_ptr;    
    if((stesoft = stestr->stesoft) == NULL){                        
        /* 起こってはいけない事態 */
        freemsg(mp);
        cmn_err(CE_CONT, "ste_send_up: stream for sted has not yet attached to PPA");        
        return;
    }

    etherhdr = (struct ether_header *)mp->b_rptr;

    DEBUG_PRINT((CE_CONT, "ste_send_up: ether_type = 0x%x\n", ntohs(etherhdr->ether_type)));
    
    /*
     * エンディアン対策
     */
    sap = ntohs(etherhdr->ether_type);

    /* ste_soft 構造体より PPA（インスタンス番号）を得る */
    ppa = stesoft->instance;
    /*
     * このインスタンスにアタッチしている stream のリスト(str_list_head)を廻り、SAP
     * 値が合致する stream にメッセージを put(9E) してまわる。また、promiscuous モード
     * が ON になっている stream にもコピーしたメッセージを put(9E) する。
     * ループの途中で、stream がなくなってしまうと困る（PANIC!）ので、各 ste_str
     * 構造体を参照前に必ず STESTR_HOLD を使ってリファレンスカウントを増し、他の thread
     * が stream を close しないことを保証する。
     */
    prevstrp = &(stesoft->str_list_head);
    STESTR_HOLD(prevstrp);
    while(prevstrp->inst_next){
        strp = prevstrp->inst_next;
        STESTR_HOLD(strp);

        if(ste_stream_is_eligible(strp, etherhdr, sap) == 0){
            STESTR_RELEASE(prevstrp);
            prevstrp = strp;            
            continue;
        }

        /*
         * putmsg(9F) に渡すメッセージを選択。
         * RAW モードの stream の場合はそのまんまのメッセージを渡す。
         * そうでなければ Ethernet ヘッダを除き（rptr をずらす）、
         * dl_unitdata_ind メッセージを追加したメッセージを渡す。
         */
        if(strp->flags & STE_RAWMODE){
            /*
             * RAW モード
             */
            tmpmp = mp;
#ifdef DEBUG
            rptr = tmpmp->b_rptr;
            datalen = tmpmp->b_wptr - tmpmp->b_rptr;
#endif            
        } else {
            /*
             * 必要になった時に1度だけ dl_unitdata_ind のメッセージを作成し、
             * 以降はコピーしたものを使っていく。
             */
            if(unitmp == NULL){
                if((unitmp = ste_create_dl_unitdata_ind(mp)) == NULL){
                    cmn_err(CE_CONT, "ste_send_up: can't create dl_unit_data_ind");
                    /* この stream への配送はあきらめるが、ループ処理は継続する。*/
                    STESTR_RELEASE(prevstrp);
                    prevstrp = strp;
                    continue;
                }
            }
            tmpmp = unitmp;
#ifdef DEBUG
            rptr = tmpmp->b_cont->b_rptr;
            datalen = tmpmp->b_cont->b_wptr - tmpmp->b_cont->b_rptr;
#endif            
        }
        
        if(canputnext(strp->qptr) == 0){
            DEBUG_PRINT((CE_CONT, "ste_send_up: can't putnext (PPA=%d,SAP=0x%x)", strp->stesoft->instance, strp->sap));
            STESTR_RELEASE(prevstrp);
            prevstrp = strp;            
            continue;
        }
        
        if((dupmp = dupmsg(tmpmp)) == NULL){
            cmn_err(CE_CONT, "ste_send_up: dupmsg failed");
            STESTR_RELEASE(prevstrp);
            prevstrp = strp;                            
            continue;
        }

#ifdef DEBUG
        /*
         * DEBUG 有効時に次の module に渡す先頭 32byte だけを出力。
         */
        DEBUG_PRINT((CE_CONT, "data length: %d\n", datalen));
        DEBUG_PRINT((CE_CONT,
          "00: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
          rptr[0],rptr[1],rptr[2],rptr[3],rptr[4],rptr[5],rptr[6],rptr[7],
          rptr[8],rptr[9],rptr[10],rptr[11],rptr[12],rptr[13],rptr[14],rptr[15]));
        DEBUG_PRINT((CE_CONT,
          "16: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
          rptr[16],rptr[17],rptr[18],rptr[19],rptr[20],rptr[21],rptr[22],rptr[23],
          rptr[24],rptr[25],rptr[26],rptr[27],rptr[28],rptr[29],rptr[30],rptr[31]));
#endif
        /*
         * 見つかった queue にメッセージを put。
         */
        putnext(strp->qptr, dupmp);
        DEBUG_PRINT((CE_CONT, "ste_send_up: putnext() called (PPA=%d,SAP=0x%x) to %s",
                     strp->stesoft->instance, strp->sap,
                     strp->qptr->q_next->q_qinfo->qi_minfo->mi_idname));

        STESTR_RELEASE(prevstrp);
        prevstrp = strp;        
        continue;
    }
    STESTR_RELEASE(prevstrp);
    freemsg(mp);
    if(unitmp)
        freemsg(unitmp);
    return;
}

/*****************************************************************************
 * ste_wput() !CHECKED!
 * 
 * ste の write サイドの put(9E) ルーチン
 * 
 * o M_PROTO メッセージの場合は IP や ARP からのデータはと考えられるので
 *   メッセージを ste_proto_wput() に渡す。
 * 
 * o M_DATA メッセージの場合は、以下の２つの可能性がある。
 *    1. sted デーモンからのデータ
 *    2. RAW モードの stream からのデータ
 *****************************************************************************/
static int
ste_wput(queue_t *q, mblk_t *mp)
{ 
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
    unsigned char dbtype;

    stestr = q->q_ptr;
    dbtype = mp->b_datap->db_type;
    
    switch(dbtype){
        case M_PROTO:
        case M_PCPROTO:
            return(ste_proto_wput(q, mp));
        case M_IOCTL:
            return(ste_ioctl_wput(q, mp));
        case M_FLUSH:
            if (*mp->b_rptr & FLUSHW) 
                flushq(q, FLUSHALL);
            if (*mp->b_rptr & FLUSHR) 
                flushq(RD(q), FLUSHALL);
            return(0);
        case M_CTL:
            /*
             * M_CTL メッセージにはなにをすればいいんだ？
             */
            DEBUG_PRINT((CE_CONT, "ste_wput: get M_CTL message. freeing message."));
            freemsg(mp);
            return(0);
        case M_DATA:
            DEBUG_PRINT((CE_CONT, "ste_wput: get M_DATA message"));
            ste_data_wput(q, mp);
            return(0);
        default:
            break;
    } /* end of switch */
    DEBUG_PRINT((CE_CONT,"ste_wput: unknown msg type(0x%x)",dbtype));
    freemsg(mp);
    return(0);
}

/*****************************************************************************
 * ste_wsrv() !CHECKED!
 *
 * ste の write サイドの srv(9E) ルーチン
 * STREAMS スケジューラーから自動的に呼ばれる。IP および ARP からの DL_UNITDATA_REQ
 * プリミティブのメッセージは sted の stream に直接 put(9F) されることなく、
 * putq(9F) により一旦 queue におかれて、この srv(9E) ルーチンから改めて sted
 * の stream に渡される。
 * これは recursive mutex_enter panic を避けるための処置・・
 *****************************************************************************/
static int
ste_wsrv(queue_t *q)
{ 
    mblk_t *mp; /* sted に渡す queuing されていたメッセージ */
    mblk_t *dupmp; /* 上位モジュールへの配信用 */    
    unsigned char dbtype;
    
    while (mp = getq(q)){
        dbtype = mp->b_datap->db_type;
        switch(dbtype){
            case M_DATA:
                if((dupmp = dupmsg(mp)) == NULL){
                    cmn_err(CE_CONT, "ste_wsrv: dupmsg failed");
                    freemsg(mp);
                    break;
                }
                /*
                 * IP、ARP、promiscuous モード の stream に転送する。
                 */
                DEBUG_PRINT((CE_CONT, "ste_wsrv: calling ste_send_up"));
                ste_send_up(q, dupmp);
                /*
                 * sted の stream に転送する。
                 */
                DEBUG_PRINT((CE_CONT, "ste_wsrv: calling ste_send_sted"));
                ste_send_sted(q, mp);
                break;
            default:
                cmn_err(CE_CONT,"ste_wsrv: unexpected msg type(0x%x)",dbtype);
                freemsg(mp);
        }
    }
    return(0);
}

#ifndef SOL10
/*****************************************************************************
 * ste_getinfo() !CHECKED!
 *
 * ste の getinfo(9E) ルーチン
 * オープンしている stream が PPA に Attach 済みであれば、そのデバイスインスタンス
 * の情報を返し、もしまだ PPA に Attach していないならエラーを返す。
 * （注1）第一引数の devi は使用してはならない。
 * （注2）Solaris 10 の場合 ddi_no_info(9F) で代用できるなので必要ない。
 *****************************************************************************/
static int
ste_getinfo(dev_info_t *devi, ddi_info_cmd_t infocmd, void *arg, void **result)
{ 
    int instance;
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    dev_t minor;

    /*
     * arg は今オープンしている stream の device 番号。
     */
    minor = getminor((dev_t)arg);
    DEBUG_PRINT((CE_CONT, "ste_getinfo called. (stream's minor = %d)",minor));

    if ((stestr = ste_str_find_by_minor(minor)) == NULL){
        DEBUG_PRINT((CE_CONT, "ste_getinfo: no such stream (minor = %d)",minor));
        *result = NULL;
        return (DDI_FAILURE);        
    }

    if((stesoft = stestr->stesoft) == NULL){                    
        *result = NULL;
        STESTR_RELEASE(stestr);
        return (DDI_FAILURE);                
    }
    devi = stesoft->devi;
    STESTR_RELEASE(stestr);
    
    instance = stesoft->instance;

    switch (infocmd) {
        case DDI_INFO_DEVT2DEVINFO:
            DEBUG_PRINT((CE_CONT, "ste_getinfo: DDI_INFO_DEVT2DEVINFO"));
            *result = (void *) devi;
            return(DDI_SUCCESS);
        case DDI_INFO_DEVT2INSTANCE:
            DEBUG_PRINT((CE_CONT, "ste_getinfo: DDI_INFO_DEVT2INSTANCE"));
            *result = (void *)&instance;
           return(DDI_SUCCESS);            
        default:
            break;
    }
    return (DDI_FAILURE);
}
#endif

/*****************************************************************************
 * ste_attach() !CHECKED!
 *
 * ste の attach(9E) ルーチン
 * ste をシステムに attach する。ste は実デバイスが存在しない仮想デバイス
 * なので、/kernel/drv/ste.conf がその代わりとなる。
 *    name="ste" parent="pseudo" instance=0;
 * 上記の instance=0 の 0 が仮想デバイスのインスタンスになる。
 * 以下のようにインスタンス 1、インスタンス 2、という行を追加すれば
 * いくらでも仮想 ste デバイスが作れる。(ste1 や ste2 に相当）
 *    name="ste" parent="pseudo" instance=1;
 *    name="ste" parent="pseudo" instance=2;
 *****************************************************************************/
static int 
ste_attach(dev_info_t *devi, ddi_attach_cmd_t cmd)
{ 
    int instance;
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */

    if (cmd != DDI_ATTACH){
        return (DDI_FAILURE);
    }

    instance = ddi_get_instance(devi);
    cmn_err(CE_CONT, "ste%d attached\n", instance);

    /*
     * 仮想デバイスのインスタンス毎の soft state 構造体を割り当てする
     */
    if(ddi_soft_state_zalloc(ste_soft_root, instance) != DDI_SUCCESS){
        return (DDI_FAILURE);
    }
    
    stesoft = (ste_soft_t *)ddi_get_soft_state(ste_soft_root, instance);

    stesoft->devi = devi;
    stesoft->instance = instance;

    /*
     * このインスタンス用の MAC アドレスを生成する
     */
    ste_generate_mac_addr(&stesoft->etheraddr);

    /*
     * /devicese/pseudo 以下にデバイスファイルを作成する
     * マイナーデバイス番号は 0 で、クローンデバイスにする。
     */
    if (ddi_create_minor_node(devi, DEVNAME, S_IFCHR, 0, DDI_NT_NET, CLONE_DEV) == DDI_FAILURE) {
        ddi_remove_minor_node(devi, NULL);
        return (DDI_FAILURE);
    }

    return (DDI_SUCCESS);
}


/*****************************************************************************
 * ste_detach() !CHECKED!
 *
 * ste の detach(9E) ルーチン
 * attach されていた仮想デバイスをシステムから detach する。
 * /devices 以下に作成されたマイナーノードも削除される
 *****************************************************************************/
static int 
ste_detach(dev_info_t *devi, ddi_detach_cmd_t cmd)
{ 
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
    int instance;

    if (cmd != DDI_DETACH){
        return (DDI_FAILURE);
    }

    instance = ddi_get_instance(devi);

    if (ste_instance_is_busy(instance)){
        return (DDI_FAILURE);
    }
        
    DEBUG_PRINT((CE_CONT, "ste_detach called(instance %d)\n", instance));
    stesoft = (ste_soft_t *)ddi_get_soft_state(ste_soft_root, instance);
    ddi_remove_minor_node(devi, NULL);
    ddi_soft_state_free(ste_soft_root, instance);
    return (DDI_SUCCESS);
}

/*****************************************************************************
 * ste_proto_wput() !CHECKED!
 * 
 * M_PCPROTO および M_PROTO メッセージ用の write サイド put ルーチン。
 * これらのメッセージは DLPI のプリミティブである（はず）である。
 * IP や ARP からのデータは DL_UNITDATA_REQ プリミティブとしてくる。
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 戻り値：
 *          常に 0 
 *****************************************************************************/
static int
ste_proto_wput(queue_t *q, mblk_t *mp)
{ 
    mblk_t *newmp;                
    t_uscalar_t *dl_primitive;
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */

    /*
     * queue の q_ptr より ste_str 構造体を得る
     */
    stestr = (ste_str_t *)q->q_ptr;
    
    dl_primitive = (t_uscalar_t *)mp->b_rptr;            

    switch(*dl_primitive){
        case DL_INFO_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_INFO_REQ"));
            ste_dl_info_req(q, mp);
            return(0);
        case DL_UNITDATA_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_UNITDATA_REQ"));
            ste_dl_unitdata_req(q,mp);
            return(0);
        case DL_UNBIND_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_UNBIND_REQ"));
            ste_dl_unbind_req(q, mp);
            return(0);
        case DL_ATTACH_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_ATTACH_REQ"));
            ste_dl_attach_req(q, mp);
            return(0);
        case DL_PROMISCON_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_PROMISCON_REQ"));
            ste_dl_promiscon_req(q, mp);
            return(0);
        case DL_BIND_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_BIND_REQ"));
            ste_dl_bind_req(q, mp);
            return(0);
        case DL_PHYS_ADDR_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_PHYS_ADDR_REQ"));
            ste_dl_phys_addr_req(q, mp);
            return(0);
        case DL_SET_PHYS_ADDR_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_SET_PHYS_ADDR_REQ"));
            ste_dl_set_phys_addr_req(q, mp);
            return(0);
        case DL_DETACH_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_DETACH_REQ"));
            ste_dl_detach_req(q, mp);
            return(0);
        case DL_ENABMULTI_REQ:
            DEBUG_PRINT((CE_CONT,"ste_proto_wput: get DL_ENABMULTI_REQ"));
            ste_dl_enabmulti_req(q, mp);
            return(0);
        default:
            break;
    }
    DEBUG_PRINT((CE_CONT,"ste_proto_wput: unknown primitive(0x%x)",*dl_primitive));
    ste_send_dl_error_ack(q, mp, *dl_primitive, 0, 0); 
    return(0);
}

/*****************************************************************************
 * ste_send_dl_ok_ack() !CHECKED!
 *
 * 渡されたメッセージブロック（mp） を利用して DL_OK_ACK プリミティブ
 * を作成し、アップストリーム側に返信する。
 *
 *  引数：
 *               q : write サイドの queue のポインタ
 *               mp: 上のモジュールから受信したメッセージブロックのポインタ
 *        primitive: 肯定応答を返したいプリミティブ
 * 戻り値：
 *          なし
 *****************************************************************************/
static void
ste_send_dl_ok_ack( queue_t *q, mblk_t *mp, t_uscalar_t primitive)
{ 
    dl_ok_ack_t *dl_ok_ack;

    dl_ok_ack = (dl_ok_ack_t *)mp->b_rptr;
    dl_ok_ack->dl_primitive = DL_OK_ACK;
    dl_ok_ack->dl_correct_primitive = primitive;

    mp->b_wptr = (unsigned char *)mp->b_rptr + sizeof(dl_ok_ack_t);
    mp->b_datap->db_type = M_PCPROTO;    
    qreply(q, mp);
}

/*****************************************************************************
 * ste_send_dl_error_ack() !CHECKED!
 *
 * 渡されたメッセージブロック（mp） を利用して DL_ERROR_ACK プリミティブ
 * を作成し、アップストリーム側に返信する。
 *
 *  引数：
 *              q : write サイドの queue のポインタ
 *              mp: 上のモジュールから受信したメッセージブロックのポインタ
 *       primitive: 否定応答を返したいプリミティブ
 *           dlerr: DLPI のエラー番号
 *         unixerr: UNIX の SYSTEM エラー番号
 *  戻り値：
 *          なし
 *****************************************************************************/
static void
ste_send_dl_error_ack( queue_t *q, mblk_t *mp, t_uscalar_t primitive, int dlerr, int unixerr)
{ 
    dl_error_ack_t *dl_error_ack;

    dl_error_ack = (dl_error_ack_t *)mp->b_rptr;
    dl_error_ack->dl_primitive = DL_ERROR_ACK;
    dl_error_ack->dl_error_primitive = primitive;
    dl_error_ack->dl_errno = dlerr;      /* DLPI error code */
    dl_error_ack->dl_unix_errno = unixerr; /* UNIX system error code */
    
    mp->b_datap->db_type = M_PCPROTO;    
    qreply(q, mp);
}

/*****************************************************************************
 * ste_ioctl_wput() !CHECKED!
 * 
 * M_IOCTL メッセージ用の write サイド put(9E) ルーチン。
 * 受け取る可能性のある IOCTL コマンドは
 *
 *     DLIOCRAW: RAW モードの要求
 *       REGSVC: sted デーモンの stream の登録要求（ste のオリジナル）
 *     UNREGSVC: sted デーモンの stream の登録抹消要求（ste のオリジナル）
 *
 * これ以外はすべて否定応答    
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 
 * 戻り値：
 *          常に 0 
 *****************************************************************************/
static int
ste_ioctl_wput(queue_t *q, mblk_t *mp)
{ 
    struct iocblk *iocp;
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
    mblk_t *optmp;
    struct stroptions *stropt;
    
    stestr = (ste_str_t *)q->q_ptr;    
    iocp = (struct iocblk *)mp->b_rptr;

    switch (iocp->ioc_cmd) {
        case DLIOCRAW:
            DEBUG_PRINT((CE_CONT, "ste_ioctl_wput: receive M_IOCTL message (cmd = DLIOCRAW )"));
            mutex_enter(&(stestr->lock));
            stestr->flags |= STE_RAWMODE;
            mutex_exit(&(stestr->lock));
            mp->b_datap->db_type = M_IOCACK; /* return ACK */
            iocp->ioc_count = 0;
            qreply(q, mp);
            return(0);
        case REGSVC:
            DEBUG_PRINT((CE_CONT, "ste_ioctl_wput: receive M_IOCTL message (cmd = REGSVC )"));
            if((stesoft = stestr->stesoft) == NULL){                
                /* この stream はまだ PPA にアタッチしてない！否定応答を返す */                
                DEBUG_PRINT((CE_CONT, "ste_ioctl_wput: stream has not yet attached to PPA"));
                mp->b_datap->db_type = M_IOCNAK; /* return NACK */
                iocp->ioc_count = 0;
                qreply(q, mp);                
                return(0);
            }
            /*
             * 仮想 NIC デーモンがオープンした stream の stream head の high water mark
             * 値を上げる。
             */
            if((optmp = allocb(sizeof(struct stroptions), NULL)) == NULL){
                /* allocb が失敗したら hiwat の変更を断念。否定応答を返す*/
                DEBUG_PRINT((CE_CONT, "ste_ioctl_wput: allocb for M_SETOPTS failed"));
                mp->b_datap->db_type = M_IOCNAK; /* return NACK */
                iocp->ioc_count = 0;
                qreply(q, mp);
                return(0);
            } 
            optmp->b_datap->db_type = M_SETOPTS;
            stropt = (struct stroptions *)optmp->b_rptr;
            stropt->so_flags = SO_HIWAT;
            stropt->so_hiwat = STR_HIWAT;
            qreply(q, optmp);
            /*
             * インスタンス毎の ste_soft 構造体をさわるので、lock を取得する。
             */
            mutex_enter(&(stesoft->lock));
            stesoft->svcq = RD(q);
            mutex_exit(&(stesoft->lock));

            /*
             * フラグをセットする
             */
            mutex_enter(&(stestr->lock));
            stestr->flags |= STE_SVCQ;
            mutex_exit(&(stestr->lock));
            /*
             * 物理 NIC の様に、Link up メッセージを出力する。
             * 併せて Link down の警告メッセージの累積数をリセットする。
             */
            cmn_err(CE_CONT, "ste%d: Link up -- sted started", stesoft->instance);
            stesoft->link_warning = 0;
            /*
             * IOCTL の肯定応答を返す
             */
            mp->b_datap->db_type = M_IOCACK; 
            iocp->ioc_count = 0;
            qreply(q, mp);
            return(0);
        case UNREGSVC:
            DEBUG_PRINT((CE_CONT, "ste_ioctl_wput: receive M_IOCTL message (cmd = UNREGSVC )"));
            if((stesoft = stestr->stesoft) == NULL){
                /* この stream はまだ PPA にアタッチしてない！否定応答を返す */                
                DEBUG_PRINT((CE_CONT, "ste_ioctl_wput: stream has not yet attached to PPA"));
                mp->b_datap->db_type = M_IOCNAK; /* return NACK */
                iocp->ioc_count = 0;
                qreply(q, mp);                
                return(0);
            }
            /*
             * インスタンス毎の ste_soft 構造体をさわるので、lock を取得する。
             */            
            mutex_enter(&(stesoft->lock));
            stesoft->svcq = NULL;
            mutex_exit(&(stesoft->lock));
            /*
             * フラグをはずす
             */
            mutex_enter(&(stestr->lock));
            stestr->flags &= ~STE_SVCQ;
            mutex_exit(&(stestr->lock));
            /*
             * IOCTL の肯定応答を返す
             */            
            mp->b_datap->db_type = M_IOCACK; 
            iocp->ioc_count = 0;
            qreply(q, mp);
            return(0);
        default:
            DEBUG_PRINT((CE_CONT, "ste_ioctl_wput: receive unknown M_IOCTL message (cmd = 0x%x)", iocp->ioc_cmd));
            mp->b_datap->db_type = M_IOCNAK; /* return N-ACK */
            iocp->ioc_count = 0;
            qreply(q, mp);
            return(0);
    }
}

/*****************************************************************************
 * ste_str_alloc() !CHECKED!
 *
 * ste_str 構造体の割り当てを行う。このルーチンは ste_open() から呼ばれ、
 * /dev/ste のオープン毎に新しい ste_str 構造体が割り当てられることになる。
 *
 *  引数：
 *          minor : ste_open() 内で割り当てられた stream のマイナーデバイス番号
 *              q : read サイドの queue のポインタ
 * 戻り値：
 *          成功: 割り当てられた ste_str 構造体へのポインタ
 *          失敗: NULL
 *****************************************************************************/
static ste_str_t * 
ste_str_alloc(dev_t minor, queue_t *q)
{ 
    ste_str_t  *newstr;          // 新しく確保する ste_str 構造体
    ste_str_t  *strp, *prevstrp; // 処理用のポインタ

    DEBUG_PRINT((CE_CONT,"ste_str_alloc called"));

    newstr = (ste_str_t *)kmem_zalloc(sizeof(ste_str_t), KM_NOSLEEP);
    if(newstr == NULL){
        cmn_err(CE_CONT, "ste_str_alloc: kmem_zalloc failed");
        return(NULL);
    }

    /*
     * ste_str の初期化
     */
    newstr->str_next = NULL;
    newstr->inst_next = NULL;
    newstr->qptr = q;
    newstr->minor = minor;
    newstr->dl_state = DL_UNATTACHED;
    newstr->refcnt = 0;
    newstr->flags = 0x0;
    mutex_init(&(newstr->lock), NULL, MUTEX_DRIVER, NULL);
    cv_init(&(newstr->cv), NULL, CV_DRIVER, NULL);    
    
    prevstrp = &ste_str_g_head;
    mutex_enter(&(prevstrp->lock));
    while (prevstrp->str_next != NULL){
        strp = prevstrp->str_next;
        mutex_enter(&(strp->lock));
        mutex_exit(&(prevstrp->lock));
        prevstrp = strp;
    }
    prevstrp->str_next = newstr;
    mutex_exit(&(prevstrp->lock));
    return(newstr);
}

/*****************************************************************************
 * ste_str_free()  !CHECKED!
 *
 * ste_close() から呼ばれ、以下を行う。
 * 
 *   o グローバルの ste_str 構造体のリストから指定された ste_str 構造体を抜く。
 *   o ste_str 構造体の割り当て解除を行う。
 *
 *  引数：
 *          minor : ste_open() 内で割り当てられた stream のマイナーデバイス番号
 * 戻り値：
 *          成功: 0
 *          失敗: -1
 *****************************************************************************/
static int
ste_str_free(dev_t minor)
{
    ste_str_t *strp, *prevstrp;

    prevstrp = &ste_str_g_head;
    
    mutex_enter(&(prevstrp->lock));
    while(prevstrp->str_next){
        strp = prevstrp->str_next;
        mutex_enter(&(strp->lock));
        if (strp->minor == minor){
            /*
             * リファレンスカウントが 0 になるまで cv_wait(9F)で待つ。
             * 誰もこのストリームを参照していなければ 0 のはず。
             * bug があれば ここでハングするだろう。
             */
            while(strp->refcnt != 0){
                DEBUG_PRINT((CE_CONT,"ste_str_free : refcnt(=%d) is not 0 ", strp->refcnt));                
                cv_wait(&(strp->cv), &(strp->lock));
            }
            prevstrp->str_next = strp->str_next;
            mutex_destroy(&(strp->lock));        
            cv_destroy(&(strp->cv));                        
            kmem_free(strp, sizeof(ste_str_t));
            mutex_exit(&(prevstrp->lock));
            return(0);
        }
        mutex_exit(&(prevstrp->lock));
        prevstrp = strp;
    }
    mutex_exit(&(prevstrp->lock));

    DEBUG_PRINT((CE_CONT,"ste_str_free : ste_str structure(minor=%d) not found", minor));
    return(-1);
}

/*****************************************************************************
 * ste_str_find_by_minor() !CHECKED!
 *
 * 与えられた stream のマイナーデバイス番号から ste_str 構造体を見つける。
 * 見つかった ste_str 構造体の参照カウント（refcnt）を１増加した後、構造体の
 * アドレスをリターンする。参照カウントを減少するのは呼び出し側の責任。
 *
 *  引数：
 *          minor : ste_open() 内で割り当てられた stream のマイナーデバイス番号
 * 
 * 戻り値：
 *          成功: 見つかった ste_str 構造体のポインタ
 *          失敗: NULL
 * 
 *****************************************************************************/
static ste_str_t *
ste_str_find_by_minor(dev_t minor)
{ 
    ste_str_t *strp, *prevstrp;

    /*
     * ste_str_g_head から始まる全 ste_str 構造体のリストを廻る。
     * リスト中の構造体の保護方法として、いままではグローバルロックを
     * とっていたが、個々の ste_str 構造体にロックを持たせ、個別に
     * ロックを取るようにした。（パフォーマンス向上の為）
     * str_next（次の構造体のアドレス）が変わってしまうのを避けるため、
     * 参照中のもの(strp) とその前(prevstrp)の ste_str 構造体両方のロック
     * をとるようにしている。
     */
    prevstrp = &ste_str_g_head;
    mutex_enter(&(prevstrp->lock));
    while (prevstrp->str_next != NULL){
        strp = prevstrp->str_next;
        mutex_enter(&(strp->lock));
        if (strp->minor == minor){
            /*
             * もし指定されたマイナーデバイス番号を持つ ste_str 構造体
             * が見つかったら、参照カウント(refcnt) を増やす。
             * これにより、return 後に strp が指す構造体がフリーさ
             * れてしまうのを避ける。
             */
            strp->refcnt++;
            mutex_exit(&(strp->lock));
            mutex_exit(&(prevstrp->lock));
            return(strp);
        }
        mutex_exit(&(prevstrp->lock));
        prevstrp = strp;
    }
    mutex_exit(&(prevstrp->lock));
    
    DEBUG_PRINT((CE_CONT,"ste_str_find_by_minor: ste_str structure(minor=%d) not found", minor));
    return(NULL);
}

/*****************************************************************************
 * bebug_print() !CHECKED!
 *
 * デバッグ出力用関数
 *
 *  引数：
 *           level  :  エラーの深刻度。cmn_err(9F) の第一引数に相当
 *           format :  メッセージの出力フォーマットcmn_err(9F) の第二引数に相当
 * 戻り値：
 *           なし。
 *****************************************************************************/
static void
debug_print(int level, char *format, ...)
{ 
    va_list     ap;
    char        buf[MAX_MSG];

    va_start(ap, format);
    vsprintf(buf, format, ap);    
    va_end(ap);
    cmn_err(level, "%s", buf);
}    

/*****************************************************************************
 * ste_dl_info_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_INFO_REQ プリミティブの処理を行う
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 戻り値：
 *          常に 0 
 * 
 *****************************************************************************/
static int
ste_dl_info_req(queue_t *q, mblk_t *mp)
{ 
    mblk_t *newmp;                    
    dl_info_ack_t *dl_info_ack;
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
    stedladdr_t mydladdr;

    stestr = q->q_ptr;    
    newmp = allocb(sizeof (dl_info_ack_t) + STEDLADDRL + ETHERADDRL , BPRI_HI);
    if(newmp == NULL){
        cmn_err(CE_CONT, "ste_dl_info_req: allocb failed");
        freemsg(mp);
        return(0);
    }
    dl_info_ack = (dl_info_ack_t *)newmp->b_rptr;

    dl_info_ack->dl_primitive           =  DL_INFO_ACK;
    dl_info_ack->dl_max_sdu             =  ETHERMTU;
    dl_info_ack->dl_min_sdu             =  0;
    dl_info_ack->dl_addr_length         =  STEDLADDRL;
    dl_info_ack->dl_mac_type            =  DL_ETHER;
    dl_info_ack->dl_reserved            =  0;
    dl_info_ack->dl_current_state       =  0;
    dl_info_ack->dl_sap_length          =  -2;
    dl_info_ack->dl_service_mode        =  DL_CLDLS;
    dl_info_ack->dl_qos_length          =  0;
    dl_info_ack->dl_qos_offset          =  0;
    dl_info_ack->dl_qos_range_length    =  0;
    dl_info_ack->dl_qos_range_offset    =  0;
    dl_info_ack->dl_provider_style      =  DL_STYLE2;
    dl_info_ack->dl_addr_offset         =  sizeof (dl_info_ack_t);
    dl_info_ack->dl_version             =  DL_VERSION_2;
    dl_info_ack->dl_brdcst_addr_length  =  ETHERADDRL;
    dl_info_ack->dl_brdcst_addr_offset  =  sizeof (dl_info_ack_t) + STEDLADDRL;
    dl_info_ack->dl_growth	            =  0;

    /*
     * Stream が PPA に attach しているかどうかを確認。 
     * すでに attach していれば、その instance の Ethernet アドレスを返す
     */
    if((stesoft = stestr->stesoft) == NULL){ 
        /* この stream はまだ PPA にアッタチしていない */
        bzero((char *)dl_info_ack + sizeof (dl_info_ack_t), STEDLADDRL);
    } else {
        /*
         * この stream はすでに PPA にアッタチしている。このインスタンスに
         * イーサネットアドレスをセットする。
         */
        bcopy(&stesoft->etheraddr, &mydladdr.etheraddr, ETHERADDRL);
        mydladdr.sap = (ushort_t)stestr->sap;
        bcopy(&mydladdr,(char *)dl_info_ack + sizeof (dl_info_ack_t), STEDLADDRL);
    }
    bcopy(broadcastaddr, (char *)dl_info_ack + sizeof (dl_info_ack_t) + STEDLADDRL, ETHERADDRL);
    newmp->b_wptr = (unsigned char *)newmp->b_rptr + sizeof (dl_info_ack_t) + STEDLADDRL + ETHERADDRL;

    newmp->b_datap->db_type = M_PCPROTO;
    qreply(q, newmp);
    freemsg(mp);
    return(0);
}

/*****************************************************************************
 * ste_dl_unitdata_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_UNITDATA_REQ プリミティブの処理を行う。
 * DL_UNITDATA_REQ は IP もしくは ARP の stream からのメッセージと考えられる。
 * Ethernet ヘッダーを作成し M_DATA メッセージに変更した後、putq(9F)
 * にて一旦 queue に置いておく。その後の処理は srv(9E) ルーチンである
 * ste_wsrv() によって行われる。
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 
 * 戻り値：
 *          常に 0 
 * 
 *****************************************************************************/
static int
ste_dl_unitdata_req(queue_t *q, mblk_t *mp)
{ 
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    mblk_t *datamp;
    dl_unitdata_req_t *dl_unitdata_req;
    unsigned char destaddr[6];
    struct ether_header *etherhdr;
    ste_str_t *strp; /* ste_str 構造体のリンクリストの処理用ポインタ */
    mblk_t *dupmp;   /* promis モードの stream への配信用 */
    stedladdr_t *dladdr;
    ushort_t     type;

    stestr = (ste_str_t *)q->q_ptr;
    /*
     * DLPI のステータス確認
     */
    if(stestr->dl_state != DL_IDLE){
        DEBUG_PRINT((CE_CONT,"ste_dl_unitdata_req: DLPI Status(0x%x) is invalid for this primitive\n",stestr->dl_state));
        ste_send_dl_error_ack(q, mp, DL_UNITDATA_REQ, DL_OUTSTATE, 0);
        return(0);
    }

    stesoft = stestr->stesoft;    
    if( stesoft == NULL){
        /* 起こってはいけない事態 */
        freemsg(mp);
        cmn_err(CE_CONT, "ste_dl_unitdata_req: stream has not yet attached to PPA");
        return(0);
    }
    datamp = mp->b_cont;
    if(!datamp || datamp->b_datap->db_type != M_DATA){
        /* b_cont に M_DATA message が含まれていない！*/
        freemsg(mp);
        cmn_err(CE_CONT, "ste_dl_unitdata_req: doesn't have M_DATA message");
        return(0);
    }
    /*
     * M_PROTO の dl_unitdata_req を ethernet ヘッダーとして再利用
     * wptr - rptr のサイズの確認をせねば・・・大丈夫かな？
     */
    dl_unitdata_req = (dl_unitdata_req_t *)mp->b_rptr;
    dladdr = (stedladdr_t *)((char *)dl_unitdata_req + dl_unitdata_req->dl_dest_addr_offset);
    bcopy((char *)&dladdr->etheraddr, destaddr, ETHERADDRL);

    /*
     * 802.3 フレームを要求されているかどうかを確認する。
     * もしそうならば、ethernet ヘッダーの ether_type フィールドにメッセージ長を
     * 入れる
     * 本来 SAP 値は DL_UNITDATA_REQ メッセージに含まれる DLSAP アドレス(dladdr->sap)から取ってく
     * るべきだが、x86 Solaris 9 ではなぜか DLSAP アドレスに正しい SAP 値が含まれていないので、この
     *  ように stestr->sap から取ってくるように修正した。
     */
    if ( stestr->sap <= ETHERMTU || stestr->sap == 0 )
        type = ste_msg_len(datamp);
    else
        type = stestr->sap;

    etherhdr = (struct ether_header *)mp->b_rptr;
    bcopy(destaddr, (char *)&etherhdr->ether_dhost, ETHERADDRL);
    bcopy(&stesoft->etheraddr, &etherhdr->ether_shost, ETHERADDRL);
    
    /*
     * エンディアン対策
     */
    etherhdr->ether_type = htons(type);

    mp->b_wptr = (unsigned char *)mp->b_rptr + sizeof(struct ether_header);
    mp->b_datap->db_type = M_DATA;

    putq(q, mp);
    return(0);
}

/*****************************************************************************
 * ste_dl_attach_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_ATTACH_REQ プリミティブの処理を行う
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 
 * 戻り値：
 *          常に 0 
 * 
 *****************************************************************************/
static int
ste_dl_attach_req(queue_t *q, mblk_t *mp)
{ 
    t_uscalar_t ppa;
    dl_attach_req_t *dl_attach_req;
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */

    /*
     * queue の q_ptr より ste_str 構造体を得る
     */
    stestr = (ste_str_t *)q->q_ptr;
    
    dl_attach_req = (dl_attach_req_t *)mp->b_rptr;
    ppa = dl_attach_req->dl_ppa;

    DEBUG_PRINT((CE_CONT,"ste_dl_attach_req: PPA = %d",ppa));

    /*
     * DLPI のステータス確認
     */
    if(stestr->dl_state != DL_UNATTACHED){
        DEBUG_PRINT((CE_CONT,"ste_dl_attach_req: DLPI Status(0x%x) is invalid for this primitive\n",stestr->dl_state));
        ste_send_dl_error_ack(q, mp, DL_ATTACH_REQ, DL_OUTSTATE, 0);
        return(0);
    }
    
    /*
     * PPA よりインスタンス毎の ste_soft 構造体を得る。
     */
    stesoft = (ste_soft_t *)ddi_get_soft_state(ste_soft_root, ppa);

    if( stesoft == NULL){
        DEBUG_PRINT((CE_CONT,"ste_dl_attach_req: PPA=%d doesn't exist",ppa));
        goto error;
    }
#ifdef SOL10
        /*
         * Solaris 10 で追加になったらしい。
         * See qassociate(9F) and ddi_create_minor_node(9f)
         */
    if (qassociate(q, ppa) != 0){
        DEBUG_PRINT((CE_CONT,"ste_dl_attach_req: PPA=%d doesn't exist",ppa));
        goto error;
    }
#endif    
        
    /*
     * インスタンスは存在する。ポインターを ste_str 構造体にセットする
     */
    mutex_enter(&(stestr->lock));    
    stestr->stesoft = stesoft;
    stestr->dl_state = DL_UNBOUND;
    mutex_exit(&(stestr->lock));        
    /*
     * インスタンス毎の ste_str 構造体のリストにアタッチした ste_str 構造体を追加する
     */
    ste_add_to_inst_list(&stesoft->str_list_head, stestr);
    ste_send_dl_ok_ack(q, mp, DL_ATTACH_REQ);

    return(0);

error:
    ste_send_dl_error_ack(q, mp, DL_ATTACH_REQ, DL_BADPPA, 0);
    return(0);    
}

/*****************************************************************************
 * ste_dl_detach_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_DETACH_REQ プリミティブの処理を行う
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 
 * 戻り値：
 *          常に 0 
 * 
 *****************************************************************************/
static int
ste_dl_detach_req(queue_t *q, mblk_t *mp)
{ 
    ste_str_t   *stestr; /* この stream の ste_str 構造体 */
    ste_soft_t  *stesoft;

    /*
     * queue の q_ptr より ste_str 構造体を得る
     */
    stestr = (ste_str_t *)q->q_ptr;
    stesoft = stestr->stesoft;

    /*
     * DLPI のステータス確認
     */
    if(stestr->dl_state != DL_UNBOUND){
        DEBUG_PRINT((CE_CONT,"ste_dl_detach_req: DLPI Status(0x%x) is invalid for this primitive\n",stestr->dl_state));
        ste_send_dl_error_ack(q, mp, DL_DETACH_REQ, DL_OUTSTATE, 0);
        return(0);
    }
    /*
     * インスタンス毎の ste_str 構造体のリストから抜く
     */
    ste_delete_from_inst_list(&stesoft->str_list_head, stestr);

    /*
     * インスタンス毎の ste_soft 構造体へのリンクを解除
     */
    mutex_enter(&(stestr->lock));    
    stestr->stesoft = NULL;
    /*
     * DLPI ステータスを DL_UNATTACHED に変更。
     */
    stestr->dl_state = DL_UNATTACHED;
    mutex_exit(&(stestr->lock));        

    ste_send_dl_ok_ack(q, mp, DL_DETACH_REQ);
#ifdef SOL10
        /*
         * Solaris 10 で追加になったらしい。
         * See qassociate(9F) and ddi_create_minor_node(9f)
         */
    (void) qassociate(q, -1);
#endif
    
    return(0);
}

/*****************************************************************************
 * ste_dl_bind_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_BIND_REQ プリミティブの処理を行う
 * SAP 値（インスタンス番号）を ste_str 構造体に保存しておく。
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 
 * 戻り値：
 *          常に 0 
 *****************************************************************************/
static int
ste_dl_bind_req(queue_t *q, mblk_t *mp)
{ 
    dl_bind_req_t *dl_bind_req;
    dl_bind_ack_t *dl_bind_ack;
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
    mblk_t *newmp;
    stedladdr_t mydladdr;
        
    dl_bind_req = (dl_bind_req_t *)mp->b_rptr;

    DEBUG_PRINT((CE_CONT,"ste_dl_bind_req: SAP = 0x%x",dl_bind_req->dl_sap));
    /*
     * queue の q_ptr より ste_str 構造体を得る
     */
    stestr = (ste_str_t *)q->q_ptr;

    /*
     * DLPI のステータス確認
     */
    if(stestr->dl_state != DL_UNBOUND){
        DEBUG_PRINT((CE_CONT,"ste_dl_bind_req: DLPI Status(0x%x) is invalid for this primitive\n",stestr->dl_state));
        ste_send_dl_error_ack(q, mp, DL_BIND_REQ, DL_OUTSTATE, 0);
        return(0);
    }
    /*
     * SAP 値（インスタンス番号）を ste_str 構造体に保存しておく。
     */
    mutex_enter(&(stestr->lock));
    stestr->sap = dl_bind_req->dl_sap;
    mutex_exit(&(stestr->lock));    

    newmp = allocb(sizeof (dl_bind_ack_t) + STEDLADDRL, BPRI_HI);
    if(newmp == NULL){
        cmn_err(CE_CONT, "ste_dl_bind_req: allocb failed");
        freemsg(mp);
        return(0);
    }        
        
    dl_bind_ack = (dl_bind_ack_t *)newmp->b_rptr;
    dl_bind_ack->dl_primitive = DL_BIND_ACK;
    dl_bind_ack->dl_sap = dl_bind_req->dl_sap;
    dl_bind_ack->dl_addr_length = STEDLADDRL;
    dl_bind_ack->dl_addr_offset = sizeof(dl_bind_ack_t);
    dl_bind_ack->dl_max_conind = 0; 
    dl_bind_ack->dl_xidtest_flg = 0;

    /*
     * インスタンス毎の ste_soft 構造体を確認する。
     */
    if( stestr->stesoft == NULL){
        /*
         * アタッチしているはずのステータスなのに、ste_soft がリンクされていない
         * dl_state がおかしくなっていると思われる。
         */
        DEBUG_PRINT((CE_CONT,"ste_dl_bind_req: DLPI states might be corrupted.\n"));
        ste_send_dl_error_ack(q, mp, DL_BIND_REQ, DL_SYSERR, 0);
        return(0);
    }
    stesoft = stestr->stesoft;
    bcopy(&stesoft->etheraddr, &mydladdr.etheraddr, ETHERADDRL);
    mydladdr.sap = (ushort_t)stestr->sap;
    bcopy(&mydladdr, (char *)dl_bind_ack + sizeof (dl_bind_ack_t), STEDLADDRL);

    newmp->b_datap->db_type = M_PCPROTO;
    newmp->b_wptr = (unsigned char *)newmp->b_rptr + sizeof(dl_bind_ack_t) + STEDLADDRL;
    qreply(q, newmp);
    freemsg(mp);

    mutex_enter(&(stestr->lock));        
    stestr->dl_state = DL_IDLE;
    mutex_exit(&(stestr->lock));
    
    return(0);
}
/*****************************************************************************
 * ste_dl_unbind_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_UNBIND_REQ プリミティブの処理を行う
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 
 * 戻り値：
 *          常に 0 
 *****************************************************************************/
static int
ste_dl_unbind_req(queue_t *q, mblk_t *mp)
{ 
    dl_unbind_req_t *dl_bind_req;
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
        
    /*
     * queue の q_ptr より ste_str 構造体を得る
     */
    stestr = (ste_str_t *)q->q_ptr;

    /*
     * DLPI のステータス確認
     */
    if(stestr->dl_state != DL_IDLE){
        DEBUG_PRINT((CE_CONT,"ste_dl_unbind_req: DLPI Status(0x%x) is invalid for this primitive\n",stestr->dl_state));
        ste_send_dl_error_ack(q, mp, DL_UNBIND_REQ, DL_OUTSTATE, 0);
        return(0);
    }
    /*
     * SAP 値を初期化し、ステータスを DL_UNBOUND へ変更
     */
    mutex_enter(&(stestr->lock));
    stestr->sap = 0;
    stestr->dl_state = DL_UNBOUND;
    mutex_exit(&(stestr->lock));    
    
    ste_send_dl_ok_ack(q, mp, DL_UNBIND_REQ);            
    return(0);
}

/*****************************************************************************
 * ste_dl_phys_addr_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_PHYS_ADDR_REQ プリミティブの処理を行う
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 戻り値：
 *          常に 0 
 *****************************************************************************/
static int
ste_dl_phys_addr_req(queue_t *q, mblk_t *mp)
{ 
    mblk_t *newmp;
    dl_phys_addr_ack_t *dl_phys_addr_ack;
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
    
    newmp = allocb(sizeof (dl_phys_addr_ack_t) + ETHERADDRL, BPRI_HI);
    if(newmp == NULL){
        cmn_err(CE_CONT, "ste_dl_phys_addr_req: allocb failed");
        freemsg(mp);
        return(0);
    }        
    
    dl_phys_addr_ack = (dl_phys_addr_ack_t *)newmp->b_rptr;
    /*
     * queue の q_ptr より ste_str 構造体を得る
     */
    stestr = (ste_str_t *)q->q_ptr;

    dl_phys_addr_ack->dl_primitive = DL_PHYS_ADDR_ACK;
    dl_phys_addr_ack->dl_addr_length = ETHERADDRL; /* length of the physical addr */
    dl_phys_addr_ack->dl_addr_offset = sizeof(dl_phys_addr_ack_t);

    /*
     * DLPI のステータス確認
     * DL_UNATTACHED（未アッタチ）以外であれば良い。
     */
    if(stestr->dl_state == DL_UNATTACHED){
        DEBUG_PRINT((CE_CONT,"ste_dl_phys_addr_req: DLPI Status(0x%x) is invalid for this primitive\n",
                     stestr->dl_state));
        ste_send_dl_error_ack(q, mp, DL_PHYS_ADDR_REQ, DL_OUTSTATE, 0);
        return(0);
    }
    /*
     * インスタンス毎の ste_soft 構造体を確認。
     * もし NULL なら、dl_state が異常ということになる。
     */
    if( stestr->stesoft == NULL){
        /* dl_state がおかしくなっていると思われる。 */
        DEBUG_PRINT((CE_CONT,"ste_dl_phys_addr_req: DLPI states might be corrupted.\n"));
        ste_send_dl_error_ack(q, mp, DL_PHYS_ADDR_REQ, DL_SYSERR, 0);
        return(0);
    }
    /* アッタチしているインスタンスの Ethernet Address を返す */
    stesoft = stestr->stesoft;
    bcopy(stesoft->etheraddr.ether_addr_octet,(char *)dl_phys_addr_ack + sizeof (dl_phys_addr_ack_t), ETHERADDRL);
    
    newmp->b_datap->db_type = M_PCPROTO;
    newmp->b_wptr = (unsigned char *)newmp->b_rptr + sizeof(dl_phys_addr_ack_t) + ETHERADDRL;

    qreply(q, newmp);
    freemsg(mp);
    return(0);
}

/*****************************************************************************
 * ste_dl_set_phys_addr_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_SET_PHYS_ADDR_REQ プリミティブの処理を行う
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 戻り値：
 *          常に 0 
 *****************************************************************************/
static int
ste_dl_set_phys_addr_req(queue_t *q, mblk_t *mp)
{ 
    mblk_t *newmp;
    dl_set_phys_addr_req_t *dl_set_phys_addr_req;
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */

    stestr = q->q_ptr;
    dl_set_phys_addr_req = (dl_set_phys_addr_req_t *)mp->b_rptr;    

    /*
     * DLPI のステータス確認
     * DL_UNATTACHED（未アッタチ）以外であれば良い。
     */
    if(stestr->dl_state == DL_UNATTACHED){
        DEBUG_PRINT((CE_CONT,"ste_dl_set_phys_addr_req: DLPI Status(0x%x) is invalid for this primitive\n",
                     stestr->dl_state));
        ste_send_dl_error_ack(q, mp, DL_SET_PHYS_ADDR_REQ, DL_OUTSTATE, 0);
        return(0);
    }
    if((stesoft = stestr->stesoft) == NULL){
        /* dl_state がおかしくなっていると思われる。 */        
        DEBUG_PRINT((CE_CONT,"ste_dl_set_phys_addr_req: DLPI states might be corrupted.\n"));
        ste_send_dl_error_ack(q, mp, DL_SET_PHYS_ADDR_REQ, DL_SYSERR, 0);
        return(0);
    }
    bcopy((char *)dl_set_phys_addr_req + sizeof (dl_set_phys_addr_req_t),stesoft->etheraddr.ether_addr_octet, ETHERADDRL);
    ste_send_dl_ok_ack(q, mp, DL_SET_PHYS_ADDR_REQ);
    return(0);
}

/*****************************************************************************
 * ste_dl_promiscon_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_PROMISCON_REQ プリミティブの処理を行う
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 戻り値：
 *          常に 0 
 *****************************************************************************/
static int
ste_dl_promiscon_req(queue_t *q, mblk_t *mp)
{ 
    ste_str_t *stestr; /* この stream の ste_str 構造体 */

    /*
     * queue の q_ptr より ste_str 構造体を得る
     */
    stestr = (ste_str_t *)q->q_ptr;

    mutex_enter(&(stestr->lock));
    stestr->flags |= STE_PROMISCON;
    mutex_exit(&(stestr->lock));
    
    ste_send_dl_ok_ack(q, mp, DL_PROMISCON_REQ);            
    return(0);
}

/*****************************************************************************
 * ste_dl_enabmulti_req() !CHECKED!
 * 
 * M_PROTO メッセージ の DL_ENABMULTI_REQ プリミティブの処理を行う。
 * 常に肯定応答を返す。ste ドライバは DL_ENABMULTI_REQ で要求された
 * アドレスに関係なく全てのマルチキャストフレーム(あて先 Ethernet
 * アドレスの先頭から 8 ビット目が 1 であるもの）を IP に転送する。
 * （ste_send_up() 参照）
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 戻り値：
 *          常に 0 
 *****************************************************************************/
static int
ste_dl_enabmulti_req(queue_t *q, mblk_t *mp)
{ 
    ste_str_t *stestr;  /* この stream の ste_str 構造体 */

    /*
     * queue の q_ptr より ste_str 構造体を得る
     */
    stestr = (ste_str_t *)q->q_ptr;
    
    ste_send_dl_ok_ack(q, mp, DL_ENABMULTI_REQ);            
    return(0);
}
/*****************************************************************************
 * ste_data_wput()  !CHECKED!
 * 
 * M_DATA メッセージ用の write サイド put(9E) ルーチン。
 * putq(9F) にて一旦 queue に置いておく。その後の処理は srv(9E) ルーチンである
 * ste_wsrv() によって行われる。
 *
 *  引数：
 *           q : write サイドの queue のポインタ
 *           mp: 受信したメッセージブロックのポインタ
 * 
 * 戻り値：
 *          常に 0 
 *****************************************************************************/
static int
ste_data_wput(queue_t *q, mblk_t *mp)
{ 
    putq(q, mp);
    return(0);
}

/*****************************************************************************
 * ste_send_sted()!CHECKED!
 *
 * 渡されたメッセージを sted がオープンした stream に put(9F) する。
 *
 *  引数：
 *            q : write サイドの queue のポインタ
 *            mp: Ethernet ヘッダー込みのメッセージブロックのポインタ
 * 戻り値：
 *        無し
 *****************************************************************************/
static void
ste_send_sted(queue_t *q, mblk_t *mp)
{ 
    ste_str_t *stestr;   /* この stream の ste_str 構造体 */
    ste_soft_t *stesoft; /* ste デバイスのインスタンスの構造体 */
    queue_t    *svcq;    /* sted の stream の queue */
    ste_str_t  *svcqstr; /* sted の stream の ste_str 構造体 */
    
    stestr = q->q_ptr;    

    if((stesoft = stestr->stesoft) == NULL){                        
        /* 起こってはいけない事態 */
        freemsg(mp);
        cmn_err(CE_CONT, "ste_send_sted: stream for sted has not yet attached to PPA");        
        return;
    }
    if( stestr->flags & STE_SVCQ ){
        /*
         * sted がオープンした stream から来た M_DATA メッセージだ。
         * sted の stream に渡す必要はない。
         */
        DEBUG_PRINT((CE_CONT,"ste_send_sted: No need to send. this is a stream opend by sted."));
        freemsg(mp);
        return;
    }
    /*
     * sted の stream がオープンしていれば、その stream に
     * メッセージを putnext(9F)。そうでなければ freemsg(9F)する。
     * 処理の間に sted の stream がクローズされてしまわないように
     * sted の stream の ste_str 構造体のリファレンスカウントをあげる
     */
    mutex_enter(&(stesoft->lock));
    svcq = stesoft->svcq;
    if(svcq == NULL){
        /* sted の stream はまだ open されていないようだ。*/
        if(stesoft->link_warning == 0){
            /* Link down のメッセージを出すのは最初の一回のみ */
            cmn_err(CE_CONT, "ste%d: Link down -- sted running?", stesoft->instance);
        }
        stesoft->link_warning++;        
        DEBUG_PRINT((CE_CONT, "ste_send_sted: STREAM for sted doesn't exist yet"));
        mutex_exit(&(stesoft->lock));        
        freemsg(mp);
        return;
    }
    svcqstr = (ste_str_t *)svcq->q_ptr;
    STESTR_HOLD(svcqstr);
    mutex_exit(&(stesoft->lock));
    
    if (canputnext(svcq) == 0){
        DEBUG_PRINT((CE_CONT, "ste_send_sted: can't putnext"));
        freemsg(mp);
        STESTR_RELEASE(svcqstr);
        return;
    }

    DEBUG_PRINT((CE_CONT, "ste_send_sted: calling putnext"));
    putnext(svcq, mp);
    STESTR_RELEASE(svcqstr);
    return;
}

/*****************************************************************************
 * ste_create_dl_unitdata_ind !CHECKED!
 *
 * 渡された M_DATA のメッセージをから、新しい DL_UNITDATA_IND プリミティブを
 * 作成し、新しいメッセージのポインタを返す。
 * 新しいメッセージのタイプは M_PCPROTO。
 *
 *  引数：
 *            mp: Ethernet ヘッダー込みのメッセージブロックのポインタ
 * 戻り値：
 *        成功時   : 新しいメッセージ(mblk)へのポインタ。
 *        エラー時 : NULL
 *
 *****************************************************************************/
static mblk_t *
ste_create_dl_unitdata_ind(mblk_t *mp)
{
    mblk_t *dupmp;
    mblk_t *newmp;
    struct ether_header *etherhdr;
    dl_unitdata_ind_t *dl_unitdata_ind;
    stedladdr_t  *dladdr;

    etherhdr = (struct ether_header *)mp->b_rptr;
    
    if((dupmp = dupmsg(mp)) == NULL){
        cmn_err(CE_CONT, "ste_create_dl_unitdata_ind: dupmsg failed");
        return(NULL);
    }    
    /* read pointer をずらし、IP もしくは ARP ヘッダを指すようにする */
    dupmp->b_rptr =  dupmp->b_rptr + sizeof(struct ether_header);    
    newmp = allocb(sizeof(dl_unitdata_ind_t) + STEDLADDRL + STEDLADDRL, BPRI_HI);
    if(newmp == NULL){
        cmn_err(CE_CONT, "ste_create_dl_unitdata_ind: allocb failed");
        freemsg(dupmp);
        return(NULL);
    }
    
    /*
     * IP や ARP にデータを渡すために、DL_UNITDAT_IND プリミティブ
     * メッセージを作成する。
     */
    dl_unitdata_ind = (dl_unitdata_ind_t *)newmp->b_rptr;
    dl_unitdata_ind->dl_primitive = DL_UNITDATA_IND;   
    dl_unitdata_ind->dl_dest_addr_length = STEDLADDRL; 
    dl_unitdata_ind->dl_dest_addr_offset = sizeof(dl_unitdata_ind_t); 
    dl_unitdata_ind->dl_src_addr_length = STEDLADDRL;
    dl_unitdata_ind->dl_src_addr_offset = sizeof(dl_unitdata_ind_t) + STEDLADDRL;
    dl_unitdata_ind->dl_group_address = 0; /* one if multicast/broadcast */

    /* unitdata_ind に宛先アドレスをセットする */
    dladdr = (stedladdr_t *)(newmp->b_rptr + dl_unitdata_ind->dl_dest_addr_offset);
    dladdr->sap = (u_short)ntohs(etherhdr->ether_type);
    bcopy((char *)&etherhdr->ether_dhost, (char *)&dladdr->etheraddr, ETHERADDRL);
    /* unitdata_ind に送信元アドレスをセットする */    
    dladdr = (stedladdr_t *)(newmp->b_rptr + dl_unitdata_ind->dl_src_addr_offset);
    dladdr->sap = (u_short)ntohs(etherhdr->ether_type);
    bcopy((char *)&etherhdr->ether_shost, (char *)&dladdr->etheraddr, ETHERADDRL);

    newmp->b_datap->db_type = M_PCPROTO;
    newmp->b_cont = dupmp;
    newmp->b_wptr = (unsigned char *)newmp->b_rptr + sizeof(dl_unitdata_ind_t) + STEDLADDRL + STEDLADDRL;
    return(newmp);
}

/*****************************************************************************
 * ste_instance_is_busy() !CHECKED!
 *
 * 引数で渡された instance にアタッチしている(state >= DL_UNATTACHED) stream
 * が存在するかどうかを確認する。もしアタッチしている stream があれば、1 を、
 * 無ければ 0 を返す。
 * ste_detach() から呼ばる。
 *
 *  引数：
 *          instance : ste のインスタンス番号
 * 戻り値：
 *          instance にアタッチしている stream が
 *            あれば   : 1
 *            無ければ : 0
 *****************************************************************************/
static int
ste_instance_is_busy(int instance)
{  
    ste_str_t *strp;
    ste_str_t *prevstrp;

    if (&ste_str_g_head == NULL){
        DEBUG_PRINT((CE_CONT,"ste_instance_is_busy: no ste_str structure exist"));
        return(0);
    }

    prevstrp = &ste_str_g_head;    
    STESTR_HOLD(prevstrp);
    while(prevstrp->inst_next){
        strp = prevstrp->inst_next;
        STESTR_HOLD(strp);
        if(strp->stesoft->instance == instance){
            DEBUG_PRINT((CE_CONT,"ste_instance_is_busy: instance %d is busy", instance));
            STESTR_RELEASE(strp);            
            STESTR_RELEASE(prevstrp);
            return(1);
        }
        STESTR_RELEASE(prevstrp);
        prevstrp = strp;        
        continue;
    }
    STESTR_RELEASE(prevstrp);    

    DEBUG_PRINT((CE_CONT,"ste_instance_is_busy: instance %d is not used", instance));
    return(0);
}

/***********************************************************************************
 * ste_is_msg_elibible() !CHECKED!
 *
 * ストリームがメッセージを渡す必要があるかものかどうかをチェックする。
 *
 * メッセージを渡す条件
 *
 *  o sted がオープンしたストリームでない。
 *  o 受信フレームの ether_type が 1500 以下で、SAP 0 にバインドしているストリーム
 *  o SAP 値が一致。
 *  o SAP 値が 0 でかつプロミスキャスモードのストリーム
 *  o 自分が出したメッセージではない。
 *  o あて先 Ethernet アドレスが自分のアドレス
 *  o あて先 Ethernet アドレスがブロードキャスト
 *  o あて先 Ethernet アドレスがマルチキャスト宛て
 *
 *  引数：
 *          
 *          
 * 戻り値：
 *          転送不要: 0
 *          転送必要: 1
 ***********************************************************************************/
static int
ste_stream_is_eligible(ste_str_t *strp, struct ether_header *etherhdr, t_uscalar_t sap)
{
    struct ether_addr *dstaddr; // 受信した Ethernet フレームに含まれるあて先
    struct ether_addr *srcaddr; // 受信した Ethernet フレームに含まれる送信元
    struct ether_addr *myaddr;  // put 先の stream のイーサネットアドレス 

    dstaddr = &etherhdr->ether_dhost;
    srcaddr = &etherhdr->ether_shost;            
    myaddr = &strp->stesoft->etheraddr;

    /* sted がオープンしたストリームなので、ここで転送する必要は無い。*/
    if ( strp->flags & STE_SVCQ){
        DEBUG_PRINT((CE_CONT, "ste_stream_is_eligible: sted daemons's stream"));         
        return(0);
    }

    /* プロミスキャスモードのストリームなら有無をいわさず OK */
    if(strp->sap == 0 && (strp->flags & STE_PROMISCON)){
        DEBUG_PRINT((CE_CONT, "ste_stream_is_eligible: promisc stream"));            
        return(1);
    }

    /*
     * 受信フレームが 802.3 フレームだった場合は、SAP 値が 0 であるストリーム
     * への転送を許可する。
     */
    if ( sap <= ETHERMTU && strp->sap == 0){
        DEBUG_PRINT((CE_CONT, "ste_stream_is_eligible: 802.3 frame"));        
        return(1);
    }    

    /* SAP 値が違ったら不要 */
    if(strp->sap != sap){
        DEBUG_PRINT((CE_CONT, "ste_stream_is_eligible: SAP not match"));            
        return(0);
    }

    /* 発元MACアドレスがストリームのMACアドレスと一致したら不要 */
    if(bcmp(srcaddr->ether_addr_octet, myaddr->ether_addr_octet, ETHERADDRL) == 0){
        DEBUG_PRINT((CE_CONT, "ste_stream_is_eligible: src mac is my addr"));        
        return(0);
    }

    /* 宛先MACアドレスがストリームのMACアドレスと一致したら OK */
    if(bcmp(dstaddr->ether_addr_octet, myaddr->ether_addr_octet, ETHERADDRL)==0){
        DEBUG_PRINT((CE_CONT, "ste_stream_is_eligible: dst mac is my addr"));         
        return(1);
    }

    /* 宛先 MAC アドレスがブロードキャストアドレスだったら OK */
    if(bcmp(dstaddr->ether_addr_octet, broadcastaddr, ETHERADDRL) == 0){
        DEBUG_PRINT((CE_CONT, "ste_stream_is_eligible: dst mac is broadcast"));        
        return(1);
    }

    /* 宛先 MAC アドレスがマルチキャストアドレスだったら OK */
    if(dstaddr->ether_addr_octet[0] & 0x01) {
        DEBUG_PRINT((CE_CONT, "ste_stream_is_eligible: dst mac is multicast"));         
        return(1);
    }

    DEBUG_PRINT((CE_CONT, "ste_stream_is_eligible: dst addr not match"));             
    return(0);
}

/*****************************************************************************
 * ste_add_to_inst_list !CHECKED!
 *
 *
 *  引数：
 *      list_head: ste_soft 構造体からリンクされているインスタンス毎の
 *                 ste_str 構造体ののリストの先頭
 *         newstr: リストに追加する新しい ste_str 構造体
 *  
 * 戻り値：
 *         無し
 *****************************************************************************/
static void
ste_add_to_inst_list(ste_str_t *list_head, ste_str_t *newstr)
{
    ste_str_t  *strp, *prevstrp; // 処理用のポインタ

    DEBUG_PRINT((CE_CONT,"ste_add_to_inst_list called"));

    prevstrp = list_head;
    mutex_enter(&(prevstrp->lock));
    while (prevstrp->inst_next != NULL){
        strp = prevstrp->inst_next;
        mutex_enter(&(strp->lock));
        mutex_exit(&(prevstrp->lock));
        prevstrp = strp;
    }
    prevstrp->inst_next = newstr;
    mutex_exit(&(prevstrp->lock));
    return;
}

/*****************************************************************************
 * ste_delete_from_inst_list() !CHECKED!
 *
 *  引数：
 *      list_head: ste_soft 構造体からリンクされているインスタンス毎の
 *                 ste_str 構造体ののリストの先頭
 *         rmstr: リストから抜く ste_str 構造体
 *  
 * 戻り値：
 *          無し
 *****************************************************************************/
static void
ste_delete_from_inst_list(ste_str_t *list_head, ste_str_t *rmstestr)
{
    ste_str_t *strp, *prevstrp;

    prevstrp = list_head;
    
    mutex_enter(&(prevstrp->lock));
    while(prevstrp->inst_next){
        strp = prevstrp->inst_next;
        mutex_enter(&(strp->lock));
        if (strp == rmstestr){
            prevstrp->inst_next = strp->inst_next;
            strp->inst_next = NULL;
            mutex_exit(&(prevstrp->lock));
            mutex_exit(&(strp->lock));            
            return;
        }
        mutex_exit(&(prevstrp->lock));
        prevstrp = strp;
    }
    mutex_exit(&(prevstrp->lock));

    DEBUG_PRINT((CE_CONT,"ste_delete_from_inst_list : ste_str structure not found"));
    return;
}

/*****************************************************************************
 * ste_generate_mac_addr()
 *
 * MAC アドレスを生成する。
 * 現在時刻と起動時間からの LBOLT で MAC アドレスを生成しているので、同一のシステム
 * 上では複数の NIC が同じ MAC アドレスを持つことになる可能性が高い。
 *
 *  引数：
 *      addr: ste_soft 構造体の etheraddr のアドレス
 *  
 * 戻り値：
 *     無し
 * 
 *****************************************************************************/
static void
ste_generate_mac_addr(struct ether_addr *addr)
{
    uint_t  tm;
    uint_t  lb;

    /* time と lbolt を得て、それらをつかって MAC アドレスの後半 3 オクテットを生成する */
    tm =  (uint_t)ddi_get_time();
    lb =  (uint_t)ddi_get_lbolt();

    /* U/L ビットを 1(=local) にセット */
    addr->ether_addr_octet[0] = 0x0a;
    addr->ether_addr_octet[1] = 0x00;
    addr->ether_addr_octet[2] = 0x20;
    addr->ether_addr_octet[3] = (uchar_t)(((tm >> 16) + lb ) & 0xff);
    addr->ether_addr_octet[4] = (uchar_t)(((tm >>  8) + lb ) & 0xff);
    addr->ether_addr_octet[5] = (uchar_t)((tm + lb) & 0xff);

    DEBUG_PRINT((CE_CONT,"ste_generate_mac_addr: %x:%x:%x:%x:%x:%x\n",
        addr->ether_addr_octet[0],
        addr->ether_addr_octet[1],
        addr->ether_addr_octet[2],
        addr->ether_addr_octet[3],
        addr->ether_addr_octet[4],
        addr->ether_addr_octet[5]));
}
/*****************************************************************************
 * ste_msg_len()
 *
 * メッセージに含まれるデータ長を計算する
 *****************************************************************************/
static int
ste_msg_len(mblk_t *mp)
{
    int len = 0;
    
    do {
        len += MBLKL(mp);
    } while (( mp = mp->b_cont) != NULL);

    return(len);
}
