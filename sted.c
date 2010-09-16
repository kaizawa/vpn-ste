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
/**********************************************************
 * sted.c
 *
 * 仮想 NIC のユーザプロセスのデーモン。
 * デバイスドライバ（ste）からのデータを socket を使って
 * 仮想ハブ（stehub）が動作しているホストに転送するユーザ
 * プロセス。また、仮想 ハブからのデータをローカルの ste
 * デバイスドライバに渡す。
 *
 *    gcc sted.c sted_socket.o -lsocket -lnsl -o sted
 *
 *  Usage: sted [ -i instance] [-h hub[:port]] [ -p proxy[:port]] [-d level]
 *
 *  引数:
 *
 *    -i instance     ste デバイスのインスタンス番号
 *                    指定されなければ、デフォルトで 0(=ste0)。
 *                 
 *    -h hub[:port]   仮想ハブ（stehub）が動作するホストを指定する。
 *                    指定されなければ、デフォルトで localhost:80。
 *                    コロン(:)の後にポート番号が指定されていれば
 *                    そのポート番号に接続にいく。デフォルトは 80。
 *
 *    -p proxy[:port] 経由するプロキシサーバを指定する。
 *                    デフォルトではプロキシサーバは使われない。
 *                    コロン(:)の後にポート番号が指定されていれば
 *                    そのポート番号に接続にいく。デフォルトは 80。
 *
 *    -d level        デバッグレベル。1 以上にした場合は フォアグランド
 *                    で実行され、標準エラー出力にデバッグ情報が
 *                    出力される。デフォルトは 0。
 *
 * 変更履歴：
 *
 *  2004/12/15
 *   o recv(), send() で使うバッファーサイズを 2 Kbyte から
 *     32 Kbyteに変更した。
 *   o ste ドライバから読み込んだデータが 1514 byte だった場合
 *     に、はすぐに仮想 HUB には転送せず、バッファに溜めておき、
 *     SENDBUF_THRESHOLD（規定値 3028Kbyte)以上 になってから送信
 *     するようにした。
 *   o select() でタイムアウト（規定値 0.4 秒）するようにし、送
 *     信バッファ内に未送信のデータがあれば送信するようにした。
 *   o 仮想ハブのポート番号を指定できるようにした。
 *  2004/12/18
 *   o プロキシサーバ経由での接続をサポートした。
 *   o debug オプションを追加した。
 *   o debug レベルが 0 (デフォルト）の場合は、バックグラウンドで
 *     実行されるようにした。
 *   o debug レベルが 0 (デフォルト）の場合は、各種エラーメッセージ
 *     を syslog に出すようにした。
 *  2004/12/29
 *   o include ファイルを追加
 *  2005/1/1
 *   o socket を Non-Blocking mode に設定
 *   o recv() のエラーが EINTR、EAGAIN だった場合には無視するように変更
 *  2005/01/08
 *   o send() の EINTR、EAGAIN だった場合には無視するように変更。
 *     write サイドも select() で確認すべきか・・。
 *  2005/01/09
 *   o stehead の orglen の値が、0 以上かどうかも確認するようにした。
 *  2005/01/18
 *   o strtok(3C) の使い方が間違っていたので修正した。
 *  2005/02/01
 *   o socket に関わる関数を独立させ、sted_socket.c に記述することにした。
 *  2005/03/15
 *   o sock_stat と driver_stat を統一し、sted_stat とした。
 *  2006/04/04
 *   o DLPI 関連の関数を独立させ、dlpiutil.c に記述するすることにした。
 ***********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stropts.h>
#include <sys/stream.h>
#include <sys/dlpi.h>
#include <sys/socket.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ethernet.h>
#include <strings.h>
#include <syslog.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "sted.h"
#include "ste.h"
#include "dlpiutil.h"

int debuglevel = 0;   /* デバッグレベル。1 以上にした場合は フォアグランドで実行される */
int use_syslog = 0;  /* メッセージを STDERR でなく、syslog に出力する */

int open_ste(stedstat_t *, char *, int);
int read_ste(stedstat_t *);
int write_ste(stedstat_t *);
int become_daemon();

int
main(int argc, char *argv[])
{
    int  ste_fd, sock_fd;
    int c, ret;
    struct fd_set fds;
    int instance = 0;  /* インターフェースのインスタンス番号。*/
    int hub_port = 0;  /* 仮想ハブのポート番号 */
    char *hub = NULL;
    char *proxy= NULL;
    char localhost[] = "localhost:80";
    char dummy;
    struct timeval timeout;
    stedstat_t stedstat[1];
    
    while ((c = getopt(argc, argv, "d:i:h:p:")) != EOF){
        switch (c) {
            case 'i':
                instance = atoi(optarg);                
                break;
            case 'h':
                hub = optarg;
                  break;
            case 'p':
                proxy = optarg;
                break;
            case 'd':
                debuglevel = atoi(optarg);
                break;
            default:
                print_usage(argv[0]);
        }
    }    

    if(hub == NULL)
        hub = localhost;

    /* syslog のための設定。Facility は　LOG_USER とする */
    openlog(basename(argv[0]),LOG_PID,LOG_USER);

    /* ste の stream  をオープン */
    if ((ste_fd = open_ste(stedstat, STEPATH, instance)) < 0){
        print_err(LOG_ERR,"Failed to open %s(instance:%d)\n",STEPATH, instance);
        goto err;
    }
    
    /* HUB との間の Connection をオープン */
    if ((sock_fd = open_socket(stedstat, hub, proxy)) < 0){
        print_err(LOG_ERR,"failed to open connection with hub\n");
        goto err;
    }

    /*
     * ここまではとりあえず、フォアグラウンドで実行。
     * ここからは、デバッグレベル 0 （デフォルト）なら、バックグラウンド
     * で実行し、そうでなければフォアグラウンド続行。
     */
    if(debuglevel == 0){
        print_err(LOG_NOTICE,"Going to background mode\n");        
        if(become_daemon() != 0){
            print_err(LOG_ERR,"can't become daemon\n");            
            goto err;
        }
    }

    FD_ZERO(&fds);
    
    while(1){
        FD_SET(ste_fd, &fds );
        FD_SET(sock_fd, &fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = SELECT_TIMEOUT;
        
        if( (ret = select(FD_SETSIZE, &fds, NULL, NULL, &timeout)) < 0){
            print_err(LOG_ERR,"select:%s\n", strerror(errno));
            goto err;
        } else if ( ret == 0 && stedstat->sendbuflen > 0 ){
            /*
             * SELECT_TIMEOUT 間に送受信がなければ、送信バッファーのデータを
             * 送信する。
             */
            if(debuglevel > 1){
                print_err(LOG_DEBUG, "select timeout(sendbuflen = %d)\n", stedstat->sendbuflen);
            }
            if (write_socket(stedstat) < 0){
                    goto err;
            }
            continue;
        }
        /* HUB からのデータ */
        if(FD_ISSET(sock_fd, &fds)){
            if(read_socket(stedstat) < 0){
                /* socket にエラーが発生した模様。再接続に行く */                
                close(sock_fd);
                stedstat->sock_fd = -1;
                if ((sock_fd = open_socket(stedstat, hub, proxy)) < 0){
                    print_err(LOG_ERR,"failed to re-open connection with hub\n");
                    goto err;
                }
            }
            continue;
        }
        /* ste ドライバからのデータ */
        if(FD_ISSET(ste_fd, &fds)){
            if(read_ste(stedstat) < 0){
                /* socket にエラーが発生した模様。再接続に行く */
                close(sock_fd);
                stedstat->sock_fd = -1;                
                if ((sock_fd = open_socket(stedstat, hub, proxy)) < 0){
                    print_err(LOG_ERR,"failed to re-open connection with hub\n");
                    goto err;
                }                
            }
            continue;
        }
    } /* main loop end */

  err:
    /*
     * もし /dev/ste をまだオープンしているなら、まず登録解除してから
     * 終了する。
     */
    if(ste_fd > 0)
        strioctl(ste_fd, UNREGSVC, -1, sizeof(int), (char *)&dummy);
    print_err(LOG_ERR,"Stopped\n");
    exit(1);
}

/*****************************************************************************
 * read_ste()
 * 
 * ste ドライバからのデータを読み込み、HUB(stehub) に転送する。
 *
 *  引数：
 *           stedstat : sted 管理構造体
 *
 * 戻り値：
 *          正常時 : 0
 *          障害時 : -1
 *****************************************************************************/
int
read_ste(stedstat_t *stedstat)
{
    struct strbuf rdata;
    stehead_t steh;
    int flags = 0;    
    int pad = 0;    /* パディング */
    int remain = 0; /* 全データ長を 4 で割った余り */
    int ret;
    uchar_t *sendbuf  = stedstat->sendbuf;    /* Socket 送信バッファ*/
    uchar_t *rdatabuf = stedstat->rdatabuf; /* ドライバからの読み込み用バッファ */
    int ste_fd        = stedstat->ste_fd;   /* 仮想 NIC デバイスをオープンした FD */
    uchar_t *sendp;   /* Socket 送信バッファの書き込み位置ポインタ */
    int readsize;

    sendp = sendbuf + stedstat->sendbuflen;    
    rdata.buf = (char *)rdatabuf;        
    rdata.maxlen = STRBUFSIZE;
    rdata.len = 0;

    ret = getmsg(ste_fd, NULL, &rdata, &flags);

    if ((ret & (MORECTL | MOREDATA)) == (MORECTL | MOREDATA))        
        print_err(LOG_NOTICE, "getmsg() returns MOREDATA or MORECTL\n");

    readsize = rdata.len;

    if (debuglevel > 1){
        print_err(LOG_DEBUG,"========= from ste %d bytes ==================\n",readsize);
        if(debuglevel > 2){
            int i;
            for (i = 0; i < readsize; i++){
                if((i)%16 == 0){
                    print_err(LOG_DEBUG,"\n%04d: ", i);                    
                }
                print_err(LOG_DEBUG, "%02x ", rdatabuf[i] & 0xff);                
            }
            print_err(LOG_DEBUG, "\n\n");
        }
    }
            
    if( remain = ( sizeof(stehead_t) + readsize ) % 4 )
        pad = 4 - remain;
    steh.len = htonl(readsize + pad);
    steh.orglen = htonl(readsize);
    if(debuglevel > 1){
        print_err(LOG_DEBUG, "stehead.len    = %d\n", ntohl(steh.len));
        print_err(LOG_DEBUG, "stehead.pad    = %d\n", pad);                                    
        print_err(LOG_DEBUG, "stehead.orglen = %d\n", ntohl(steh.orglen));
    }
    memcpy(sendp, &steh, sizeof(stehead_t));
    memcpy(sendp + sizeof(stehead_t), rdata.buf, readsize);
    memset(sendp + sizeof(stehead_t) + readsize, 0x0, pad);
    stedstat->sendbuflen += sizeof(stehead_t) + readsize + pad;

    /*
     * ste から受け取ったサイズが ETHERMAX(1514byte)より小さいか、
     * 送信バッファへの書き込み済みサイズが SENDBUF_THRESHOLD 以上になったら送信する
     */
    if( readsize < ETHERMAX || stedstat->sendbuflen > SENDBUF_THRESHOLD){
        if(debuglevel > 1){        
            print_err(LOG_DEBUG, "readsize = %d, sendbuflen = %d\n",
                      readsize, stedstat->sendbuflen);
        }
        if ( write_socket(stedstat) < 0){
            return(-1);
        }
    }
    return(0);
}

/*****************************************************************************
 * open_ste()
 * 
 * /dev/ste をオープンし、PPA（インスタンス番号）にアタッチする。
 * また、オープンされた stream を ste ドライバ内で保持するために
 * IOCTL コマンドの REGSVC（ste オリジナル）を発行する。
 *
 *  引数：
 *           stedstat :  sted 管理構造体
 *           devname    : デバイス名(/dev/ste)
 *           ppa        : PPA（仮想 NIC のインスタンス番号）
 * 戻り値：
 *         正常時   : ファイルディスクリプタ
 *         エラー時 :  -1
 *****************************************************************************/
int
open_ste(stedstat_t *stedstat, char *devname, int ppa)
{
    int ste_fd;
    char dummy;
    uchar_t *rdatabuf = stedstat->rdatabuf; /* ドライバからの読み込み用バッファ*/
    
    ste_fd = open(devname, O_RDWR, 0666);
    if ( ste_fd < 0){
        print_err(LOG_ERR, "open: %s\n", strerror(errno));                            
        return(-1);
    }

    stedstat->ste_fd = ste_fd;
    
    if(dlattachreq(ste_fd, ppa, (char *)rdatabuf) < 0){
        close(ste_fd);
        print_err(LOG_ERR, "dlattach:error\n");//todo
        return(-1);
    }

    /*
     * read queue を flash するよう要求。
     */
    if (ioctl(ste_fd, I_FLUSH, FLUSHR) < 0){
        close(ste_fd);
        print_err(LOG_ERR, "ioctl:I_FLUSH:%s\n", strerror(errno));
        return(-1);
    }

    if (strioctl(ste_fd, REGSVC, -1, sizeof(int), (char *)&dummy) < 0 ){
        close(ste_fd);
        return(-1);
    }
    return(ste_fd);
}


/***********************************************************
 * print_err()
 *
 * エラーメッセージを表示するルーチン。
 * 
 ***********************************************************/
void
print_err(int level, char *format, ...)
{
    va_list ap;
    char buf[ERR_MSG_MAX];
    
    va_start(ap, format);
    vsnprintf(buf,ERR_MSG_MAX, format, ap);
    va_end(ap);

    if(use_syslog)
        syslog(level, buf);
    else
        fprintf(stderr, buf);
}

/*****************************************************************************
 * print_usage()
 * 
 * Usage を表示し、終了する。
 *****************************************************************************/
void
print_usage(char *argv)
{
    printf ("Usage: %s [ -i instance] [-h hub[:port]] [ -p proxy[:port]] [-d level]\n",argv);
    printf ("\t-i instance     : Instance number of the ste device\n");
    printf ("\t-h hub[:port]   : Virtual HUB and its port number\n");
    printf ("\t-p proxy[:port] : Proxy server and its port number\n");
    printf ("\t-d level        : Debug level[0-3]\n");
    exit(0);
}
 
/*****************************************************************************
 * become_daemon()
 * 
 * 標準入出力、標準エラー出力をクローズし、バックグラウンドに移行する。
 *****************************************************************************/
int
become_daemon()
{
    chdir("/");
    umask(0);
    signal(SIGHUP,SIG_IGN);

    if( fork() == 0){
        use_syslog = 1;
        close (0);
        close (1);
        close (2);
        /* 新セッションの開始 */
        if (setsid() < 0)
            return(-1);
    } else {
        exit(0);
    }
    return(0);
}

/*****************************************************************************
 * write_ste()
 * 
 * ste ドライバにデータを書き込む
 *
 *  引数：
 *           stedstat : sted 管理構造体
 *
 * 戻り値：
 *          正常時 : 0
 *          障害時 : -1
 *****************************************************************************/
int
write_ste(stedstat_t *stedstat)
{
    struct strbuf wdata;
    int ste_fd = stedstat->ste_fd;
    int flags = 0;
    
    wdata.buf = (char *)stedstat->wdatabuf;
    wdata.maxlen = 0;
    wdata.len = stedstat->orgdatalen;
    if (putmsg(ste_fd, NULL, &wdata, flags) < 0 ){
        perror("putmsg:");
        return(-1);
    }    
    return(0);
}
