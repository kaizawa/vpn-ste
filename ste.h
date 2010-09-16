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
/*************************************************************
 * ste.h 
 *
 * ste 用ヘッダーファイル
 *
 *************************************************************/

#ifndef __STE_H
#define __STE_H

/*
 * 仮想 NIC ドライバと仮想 NIC デーモン（サービス）の通信に
 * 使われる IOCTL コマンド。
 *
 *  REGSVC    仮想 NIC デーモンを登録する 
 *  UNREGSVC  仮想 NIC デーモンを登録解除する
 *
 * Windows の IOCTL コマンドは METHOD_NEITHER を使っているので、
 * IRP は User-mode の仮想アドレス を提供する。
 * User-mode のアドレスは Parameters.DeviceIoControl.Type3InputBuffer
 * に入る。
 */
#ifdef STE_WINDOWS
/* Windows 用 */
#define REGSVC   (ULONG) CTL_CODE(FILE_DEVICE_UNKNOWN, 2, METHOD_NEITHER, FILE_ANY_ACCESS)
#define UNREGSVC (ULONG) CTL_CODE(FILE_DEVICE_UNKNOWN, 3, METHOD_NEITHER, FILE_ANY_ACCESS)
#else
/* Solaris 用 */
#define REGSVC   0xabcde0
#define UNREGSVC 0xabcde1       
#endif /* End of #ifdef STE_WINDOWS */

#ifdef _KERNEL
#ifdef  STE_WINDOWS
#include "ste_win.h"
#endif // #ifdef STE_WINDOWS
#endif // #ifdef _KERNEL

#endif // #ifndef __STE_H 
